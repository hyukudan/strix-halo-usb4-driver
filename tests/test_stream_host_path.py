# SPDX-License-Identifier: GPL-2.0-only
"""Run the actual host-path functions of stream.c against userspace doubles.

Covers what no other offline test reaches: the open/start error paths (the
sticky ENOMEM seen on node A), the frame bookkeeping allocation, the NAPI
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
         'resume_failure_fails_open_files',
         'early_rx_completion_is_kept',
         'damaged_rx_frame_fails_stream',
         'close_waits_for_tx_slot',
         'partial_io_is_accounted',
         'close_does_not_flush_rx',
         'spin_us_parameter_is_bounded',
         'damaged_close_is_not_eof', 'resume_failure_at_every_step',
         'latency_qos_follows_rings',
         'contiguous_buffers_carve_frames', 'contiguous_buffers_fall_back',
         'rx_only_poll_keeps_tx_interrupts', 'rx_only_poll_reads_and_errors',
         'host_batch_tx_limits', 'host_batch_tx_faults',
         'host_batch_rx_flush', 'host_batch_rx_faults')
# patches/experimental (0050, 0051): compiled with -DUSB4_EXPERIMENTAL.
EXPERIMENTAL_CASES = ('rx_pace_skips_descriptor_polls', 'rx_pace_zero_is_legacy',
                      'rx_pace_ignored_with_interrupts',
                      'tx_frame_bytes_splits_writes', 'tx_copy_nocache_same_bytes',
                      'tx_no_snoop_needs_uncached_copy', 'tx_path_parameters_are_bounded')
STRUCTS = ('tbstream_frame', 'tbstream_ring', 'tbstream_dev', 'tbstream_group', 'tbstream')
FUNCTIONS = ('spin_us_set', 'tbstream_qos_acquire', 'tbstream_qos_release', 'tbstream_ring_free_chunks',
             'tbstream_ring_alloc_chunks', 'tbstream_ring_free', 'tbstream_ring_available', 'tbstream_rx_start_poll',
             'tbstream_dev_napi_rx_locked', 'tbstream_dev_napi_rx', 'tbstream_dev_napi_spin',
             'tbstream_dev_irq_pin', 'tbstream_dev_irq_affinity', 'tbstream_dev_irq_affinity_restore',
             'tbstream_dev_xdomain', 'tbstream_dev_rx_callback', 'tbstream_dev_alloc_rx_buffers',
             'tbstream_frame_damaged', 'tbstream_dev_tx_callback', 'tbstream_dev_alloc_tx_buffers',
             'tbstream_dev_start',
             'tbstream_dev_tx_drained', 'tbstream_dev_poll_ring', 'tbstream_dev_stop',
             'tbstream_valid', 'tbstream_dev_valid', 'tbstream_dev_removed', 'tbstream_dev_closed',
             'tbstream_dev_detached', 'tbstream_dev_attached', 'to_tbstream_dev',
             'tbstream_dev_completed_rx',
             'tbstream_dev_submit_batch', 'tbstream_dev_consume_rx_batch',
             'tbstream_dev_consume_rx', 'tbstream_dev_alloc_tx', 'tbstream_dev_send_data',
             'tbstream_dev_send_data_batch',
             'tbstream_dev_send_close', 'tbstream_dev_lock', 'tbstream_dev_fops_read_iter',
             'tbstream_dev_fops_write_iter', 'tbstream_dev_fops_poll', 'tbstream_dev_open_timeout',
             'tbstream_dev_wait_stream', 'tbstream_dev_fops_open',
             'tbstream_dev_fops_release', 'tbstream_dev_from_group', 'tbstream_dev_alloc_in_hopid',
             'tbstream_dev_alloc_out_hopid', 'tbstream_dev_attach_stream', 'tbstream_dev_detach_stream',
             'to_tbstream_group', 'tbstream_dev_drop_item', 'tbstream_group_attach_stream',
             'tbstream_group_detach_stream', 'tbstream_suspend', 'tbstream_resume',
             'tbstream_shutdown')
EXPERIMENTAL_FUNCTIONS = ('tx_copy_set', 'tx_frame_bytes_set', 'tbstream_tx_frame_bytes', 'tbstream_tx_fill')


def experimental(text):
    """Whether stream.c carries patches/experimental (0049-0051 apply as a unit)."""
    return 'rx_poll_pace_ns' in text


def functions(text):
    """FUNCTIONS in definition order, with the experimental ones when present."""
    result = list(FUNCTIONS)
    if experimental(text):
        i = result.index('tbstream_dev_alloc_tx')
        result[i:i] = EXPERIMENTAL_FUNCTIONS
    return result


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
        cls.experimental = experimental(text)
        if 'tbstream_dev_window_command' in text:
            # The same regression cases run against actual 0049 core + stream
            # ownership decisions, not a new independent model of the policy.
            from test_stream_hybrid import build_hybrid_unit
            cls.tmp = tempfile.TemporaryDirectory(prefix='stream-hybrid-host-')
            cls.addClassCleanup(cls.tmp.cleanup)
            cls.binary = build_hybrid_unit(source.parents[2], Path(cls.tmp.name))
            return
        enum = re.search(r'^enum tbstream_frame_pdf \{\n.*?^\};\n', text, re.MULTILINE | re.DOTALL).group(0)
        parts = ['#include "stream_harness.h"\n', enum,
                 ''.join(re.findall(r'^#define TBSTREAM_DEV_[A-Z_]+\t+\d+\n', text, re.MULTILINE)),
                 # 0045: the block geometry and the parameter bits (expressions, after the numeric limits)
                 ''.join(re.findall(r'^#define TBSTREAM_DEV_(?:CHUNK_MAX_ORDER|MAX_CHUNKS|CONTIG_TX|CONTIG_RX)\t+.*\n',
                                    text, re.MULTILINE)),
                 # 0051: TX copy modes and the frame size floor
                 ''.join(re.findall(r'^#define TBSTREAM_TX_[A-Z_]+\t+.*\n', text, re.MULTILINE))]
        parts += [extract(text, 'struct', name) for name in STRUCTS]
        parts += [extract(text, 'function', name) for name in functions(text)]
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
        defines = ['-DUSB4_EXPERIMENTAL'] if cls.experimental else []
        result = subprocess.run([compiler, *defines, '-std=gnu11', '-g', '-O1', '-Wall', '-Wextra', '-Werror',
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

    def test_latency_qos_settings(self):
        text = (Path(os.environ['USB4_SOURCE_TREE']) / 'drivers/thunderbolt/stream.c').read_text()
        self.assertIn('static int latency_qos_us = -1;\n', text)
        self.assertIn('module_param(latency_qos_us, int, 0644);', text)
        self.assertIn('#define TBSTREAM_DEV_MAX_QOS_US\t\t100000\n', text)
        self.assertIn('\tsdev->cfg_latency_qos_us = -1;\n', text)
        attrs = re.search(r'^static struct configfs_attribute \*tbstream_dev_attrs\[\] = \{\n.*?^\};\n',
                          text, re.MULTILINE | re.DOTALL)
        self.assertIn('&tbstream_dev_attr_latency_qos_us,', attrs.group(0))
        store = extract(text, 'function', 'tbstream_dev_latency_qos_us_store')
        self.assertIn('-1,\n\t\t\t\t      TBSTREAM_DEV_MAX_QOS_US);', store)

    def test_contiguous_buffers_parameter(self):
        text = (Path(os.environ['USB4_SOURCE_TREE']) / 'drivers/thunderbolt/stream.c').read_text()
        self.assertIn('static unsigned int contig_buffers;\n', text)
        self.assertIn('module_param(contig_buffers, uint, 0644);', text)
        self.assertIn('#define TBSTREAM_DEV_CHUNK_MAX_ORDER\t(21 - PAGE_SHIFT)\n', text)

    def test_rx_only_poll_settings(self):
        text = (Path(os.environ['USB4_SOURCE_TREE']) / 'drivers/thunderbolt/stream.c').read_text()
        self.assertIn('&tbstream_dev_attr_rx_busy_poll,', text)
        store = extract(text, 'function', 'tbstream_dev_rx_busy_poll_store')
        self.assertLess(store.index('tbstream_dev_config_busy(sdev)'), store.index('sdev->rx_busy_poll = enabled'))
        self.assertIn('if (enabled && sdev->gpu_direct)', store)
        self.assertIn('sdev->rx_repost_batch || sdev->tx_submit_batch))',
                      extract(text, 'function', 'tbstream_dev_gpu_direct_store'))
        gpu = (Path(os.environ['USB4_SOURCE_TREE']) / 'drivers/thunderbolt/gpu_stream.c').read_text()
        self.assertIn('sdev->rx_repost_batch || sdev->tx_submit_batch || !xd', gpu)

    def test_host_batch_settings(self):
        text = (Path(os.environ['USB4_SOURCE_TREE']) / 'drivers/thunderbolt/stream.c').read_text()
        for name in ('rx_repost_batch', 'tx_submit_batch'):
            self.assertIn(f'&tbstream_dev_attr_{name},', text)
            store = extract(text, 'function', f'tbstream_dev_{name}_store')
            self.assertIn('if (value > 64)', store)
            self.assertIn('if (value && sdev->gpu_direct)', store)
            self.assertLess(store.index('tbstream_dev_config_busy(sdev)'),
                            store.index(f'sdev->{name} = value'))

    def test_rx_poll_pace_settings(self):
        if not self.experimental:
            self.skipTest('0050 is in patches/experimental')
        text = (Path(os.environ['USB4_SOURCE_TREE']) / 'drivers/thunderbolt/stream.c').read_text()
        self.assertIn('&tbstream_dev_attr_rx_poll_pace_ns,', text)
        store = extract(text, 'function', 'tbstream_dev_rx_poll_pace_ns_store')
        self.assertIn('if (value > 10000)', store)
        self.assertLess(store.index('tbstream_dev_config_busy(sdev)'), store.index('sdev->rx_poll_pace_ns = value'))
        self.assertIn('sdev->rx_poll_next = 0;', extract(text, 'function', 'tbstream_dev_start'))
        poll = extract(text, 'function', 'tbstream_dev_poll_ring')
        self.assertLess(poll.index('if (!sdev->busy_poll &&'), poll.index('sdev->rx_poll_pace_ns'))

    def run_case(self, case):
        if case in EXPERIMENTAL_CASES and not self.experimental:
            self.skipTest('patches/experimental case')
        result = subprocess.run([str(self.binary), case], capture_output=True, text=True, timeout=30,
                                env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=1:abort_on_error=1'})
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn(f'PASS {case}', result.stdout)


for _case in CASES + EXPERIMENTAL_CASES:
    setattr(StreamHostPathTests, f'test_{_case}', lambda self, case=_case: self.run_case(case))

if __name__ == '__main__':
    unittest.main()
