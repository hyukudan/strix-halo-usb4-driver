# SPDX-License-Identifier: GPL-2.0-only
"""Run the actual host-path functions of stream.c against userspace doubles.

Covers what no other offline test reaches: the open/start error paths (the
sticky ENOMEM seen on cluster1), the frame bookkeeping allocation, the NAPI
RX spin of 0011 running without the device mutex, IRQ affinity restore, and
the stream going away under open RAM-ring files (service removal, rmdir,
shutdown, reconnect) through the actual file operations.
Only the listed structures and function bodies come from the patched
stream.c; rings, DMA, the CM tunnel inventory and IRQs are models.
"""
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent
CASES = ('open_failure_is_not_sticky', 'frame_arrays_use_kvcalloc', 'spin_without_device_mutex',
         'spin_is_bounded_and_rearms', 'irq_affinity_is_restored', 'detach_stops_open_ram_rings',
         'reconnect_refuses_stale_endpoint', 'blocked_reader_fails_cleanly', 'rmdir_with_open_file',
         'detach_without_open_files', 'stop_is_idempotent', 'shutdown_detaches',
         'open_fails_fast_without_domain', 'open_fails_fast_without_xdomain', 'open_nonblock_eagain',
         'open_wait_is_bounded', 'open_waits_for_peer', 'open_wait_ends_when_xdomain_goes',
         'open_wait_is_interruptible', 'open_timeout_settings',
         'stream_deref_under_lock',
         'resume_failure_fails_open_files')
STRUCTS = ('tbstream_frame', 'tbstream_ring', 'tbstream_dev', 'tbstream_group', 'tbstream')
FUNCTIONS = ('tbstream_ring_free', 'tbstream_ring_available', 'tbstream_rx_start_poll',
             'tbstream_dev_napi_rx_locked', 'tbstream_dev_napi_rx', 'tbstream_dev_napi_spin',
             'tbstream_dev_irq_pin', 'tbstream_dev_irq_affinity', 'tbstream_dev_irq_affinity_restore',
             'tbstream_dev_xdomain', 'tbstream_dev_rx_callback', 'tbstream_dev_alloc_rx_buffers',
             'tbstream_dev_tx_callback', 'tbstream_dev_alloc_tx_buffers', 'tbstream_dev_start',
             'tbstream_dev_tx_drained', 'tbstream_dev_poll_ring', 'tbstream_dev_stop',
             'tbstream_valid', 'tbstream_dev_valid', 'tbstream_dev_removed', 'tbstream_dev_closed',
             'tbstream_dev_detached', 'tbstream_dev_attached', 'to_tbstream_dev',
             'tbstream_dev_completed_rx',
             'tbstream_dev_consume_rx', 'tbstream_dev_alloc_tx', 'tbstream_dev_send_data',
             'tbstream_dev_send_close', 'tbstream_dev_lock', 'tbstream_dev_fops_read_iter',
             'tbstream_dev_fops_write_iter', 'tbstream_dev_fops_poll', 'tbstream_dev_open_timeout',
             'tbstream_dev_wait_stream', 'tbstream_dev_fops_open',
             'tbstream_dev_fops_release', 'tbstream_dev_from_group', 'tbstream_dev_alloc_in_hopid',
             'tbstream_dev_alloc_out_hopid', 'tbstream_dev_attach_stream', 'tbstream_dev_detach_stream',
             'to_tbstream_group', 'tbstream_dev_drop_item', 'tbstream_group_attach_stream',
             'tbstream_group_detach_stream', 'tbstream_suspend', 'tbstream_resume',
             'tbstream_shutdown')


def extract(text, kind, name):
    if kind == 'struct':
        pattern = r'^struct ' + name + r' \{\n.*?^\};\n'
    else:
        pattern = r'^static[^;{}]*?\b' + name + r'\([^;{}]*?\)\n\{\n.*?^\}\n'
    match = re.search(pattern, text, re.MULTILINE | re.DOTALL)
    if not match:
        raise RuntimeError(f'{kind} {name} not found in stream.c')
    return match.group(0)


class StreamHostPathTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if 'USB4_SOURCE_TREE' not in os.environ:
            raise unittest.SkipTest('USB4_SOURCE_TREE with the patched stream.c is required')
        source = Path(os.environ['USB4_SOURCE_TREE']) / 'drivers/thunderbolt/stream.c'
        text = source.read_text()
        enum = re.search(r'^enum tbstream_frame_pdf \{\n.*?^\};\n', text, re.MULTILINE | re.DOTALL).group(0)
        parts = ['#include "stream_harness.h"\n', enum,
                 re.search(r'^#define TBSTREAM_DEV_MAX_OPEN_TIMEOUT_MS\t\d+\n', text, re.MULTILINE).group(0)]
        parts += [extract(text, 'struct', name) for name in STRUCTS]
        parts += [extract(text, 'function', name) for name in FUNCTIONS]
        parts.append('#include "stream_cases.h"\n')
        cls.tmp = tempfile.TemporaryDirectory(prefix='stream-host-')
        cls.addClassCleanup(cls.tmp.cleanup)
        tmp = Path(cls.tmp.name)
        unit = tmp / 'unit.c'
        unit.write_text('\n'.join(parts))
        compiler = os.environ.get('CC', 'cc')
        if not shutil.which(compiler):
            raise RuntimeError(f'Required C compiler unavailable: {compiler}')
        cls.binary = tmp / 'stream'
        result = subprocess.run([compiler, '-std=gnu11', '-g', '-O1', '-Wall', '-Wextra', '-Werror',
                                 '-Wno-unused-function', '-Wno-unused-parameter', '-Wno-sign-compare', '-fno-omit-frame-pointer',
                                 '-fno-pie', '-no-pie', '-fsanitize=address,undefined', '-pthread',
                                 '-I', str(ROOT), str(unit), '-o', str(cls.binary)],
                                capture_output=True, text=True)
        if result.returncode:
            raise RuntimeError(result.stdout + result.stderr)

    def test_shutdown_is_registered(self):
        text = (Path(os.environ['USB4_SOURCE_TREE']) / 'drivers/thunderbolt/stream.c').read_text()
        driver = re.search(r'^static struct tb_service_driver tbstream_driver = \{\n.*?^\};\n',
                           text, re.MULTILINE | re.DOTALL)
        self.assertIsNotNone(driver)
        self.assertRegex(driver.group(0), r'\n\t\.shutdown = tbstream_shutdown,\n')

    def test_open_timeout_defaults(self):
        text = (Path(os.environ['USB4_SOURCE_TREE']) / 'drivers/thunderbolt/stream.c').read_text()
        self.assertIn('static unsigned int open_timeout_ms = 30000;\n', text)
        self.assertIn('module_param(open_timeout_ms, uint, 0644);', text)
        self.assertIn('#define TBSTREAM_DEV_MAX_OPEN_TIMEOUT_MS\t600000\n', text)
        self.assertIn('\tsdev->cfg_open_timeout_ms = -1;\n', text)
        attrs = re.search(r'^static struct configfs_attribute \*tbstream_dev_attrs\[\] = \{\n.*?^\};\n',
                          text, re.MULTILINE | re.DOTALL)
        self.assertIn('&tbstream_dev_attr_open_timeout_ms,', attrs.group(0))
        store = extract(text, 'function', 'tbstream_dev_open_timeout_ms_store')
        self.assertIn('-1,\n\t\t\t\t      TBSTREAM_DEV_MAX_OPEN_TIMEOUT_MS);', store)

    def run_case(self, case):
        result = subprocess.run([str(self.binary), case], capture_output=True, text=True, timeout=30,
                                env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=1:abort_on_error=1'})
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn(f'PASS {case}', result.stdout)


for _case in CASES:
    setattr(StreamHostPathTests, f'test_{_case}', lambda self, case=_case: self.run_case(case))

if __name__ == '__main__':
    unittest.main()
