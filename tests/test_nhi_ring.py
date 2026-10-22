# SPDX-License-Identifier: GPL-2.0-only
"""Run the actual ring completion path of the patched nhi.c under ASan/UBSan.

ring_write_descriptors(), ring_work(), tb_ring_poll() and __ring_interrupt()
(and, with 0041, the polling collector; with 0043, the diagnostic snapshot; with 0044, the choice of the
completion workqueue) are extracted from nhi.c; the ring,
frame and descriptor layouts from the patched headers. MMIO, the workqueue
and the clock are doubles (the clock is deterministic, so the completion
path timing of 0040 is checked exactly).
"""
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent
CASES = ('irq_is_stamped', 'irq_is_stamped_with_start_poll', 'work_collection_is_stamped',
         'cancel_is_not_a_collection', 'collect_in_order_with_budget_and_refill',
         'collect_refuses_interrupt_ring', 'collect_then_stop_exactly_once', 'collect_rx_fields',
         'snapshot_reads_without_changing', 'wq_chosen_at_start_by_param', 'irq_queues_on_the_ring_wq')
FUNCTIONS = ('ring_full', 'ring_empty', 'ring_write_descriptors', 'ring_work', 'tb_ring_poll',
             'ring_select_wq', '__ring_interrupt', 'tb_ring_collect', 'tb_ring_snapshot')


def extract(text, name):
    match = re.search(r'^[a-z][^;{}\n]*\b' + name + r'\([^;{}]*?\)\n\{\n.*?^\}\n', text,
                      re.MULTILINE | re.DOTALL)
    if not match:
        raise RuntimeError(f'{name} not found')
    return match.group(0)


def block(text, start):
    """From a line starting with @start to the end of its declaration (};)."""
    match = re.search(r'^' + re.escape(start) + r'.*?^\};\n', text, re.MULTILINE | re.DOTALL)
    if not match:
        raise RuntimeError(f'{start} not found')
    return match.group(0)


def defines(text, prefix):
    return ''.join(m.group(0) for m in re.finditer(r'^#define ' + prefix + r'\w*\t+.*\n', text, re.MULTILINE))


class NhiRingTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if 'USB4_SOURCE_TREE' not in os.environ:
            raise unittest.SkipTest('USB4_SOURCE_TREE with the patched nhi.c is required')
        source = Path(os.environ['USB4_SOURCE_TREE'])
        nhi_c = (source / 'drivers/thunderbolt/nhi.c').read_text()
        header = (source / 'include/linux/thunderbolt.h').read_text()
        regs = (source / 'drivers/thunderbolt/nhi_regs.h').read_text()
        parts = ['#include "nhi_ring_harness.h"\n',
                 block(header, 'enum ring_desc_flags {'),
                 'struct tb_nhi;\nstruct ring_frame;\n',
                 block(header, 'struct tb_ring_stamps {'),
                 block(header, 'struct tb_ring_snapshot {'),
                 block(header, 'struct tb_ring {'),
                 defines(header, 'RING_FLAG_'),
                 'typedef void (*ring_cb)(struct tb_ring *, struct ring_frame *, bool canceled);\n',
                 block(header, 'struct ring_frame {'),
                 block(regs, 'struct ring_desc {'),
                 '#include "nhi_ring_doubles.h"\n']
        parts += [extract(nhi_c, name) for name in FUNCTIONS]
        parts.append('#include "nhi_ring_cases.h"\n')
        cls.tmp = tempfile.TemporaryDirectory(prefix='nhi-ring-')
        cls.addClassCleanup(cls.tmp.cleanup)
        tmp = Path(cls.tmp.name)
        unit = tmp / 'unit.c'
        unit.write_text(''.join(parts))
        compiler = os.environ.get('CC', 'cc')
        if not shutil.which(compiler):
            raise RuntimeError(f'Required C compiler unavailable: {compiler}')
        cls.binary = tmp / 'nhi_ring'
        result = subprocess.run([compiler, '-std=gnu11', '-g', '-O1', '-Wall', '-Wextra', '-Werror',
                                 '-Wno-unused-function', '-Wno-unused-parameter', '-fno-omit-frame-pointer',
                                 '-fno-pie', '-no-pie', '-fsanitize=address,undefined', '-pthread',
                                 '-I', str(ROOT), str(unit), '-o', str(cls.binary)],
                                capture_output=True, text=True)
        if result.returncode:
            raise RuntimeError(result.stdout + result.stderr)

    def test_ring_wq_is_latched_from_start_to_stop(self):
        """0044: the workqueue is chosen before the ring runs, the stop path flushes on that same one, the
        per-CPU high-priority workqueue exists for the lifetime of the NHI and no ring work goes to dma_wq
        directly any more."""
        nhi_c = (Path(os.environ['USB4_SOURCE_TREE']) / 'drivers/thunderbolt/nhi.c').read_text()
        start = extract(nhi_c, 'tb_ring_start')
        self.assertLess(start.index('ring->wq = ring_select_wq(ring->nhi);'), start.index('ring->running = true;'))
        self.assertIn('queue_work(ring->wq ? ring->wq : ring->nhi->dma_wq, &ring->work);', extract(nhi_c, 'tb_ring_stop'))
        self.assertIn('alloc_workqueue("tb_dma_hi", WQ_PERCPU | WQ_HIGHPRI | WQ_MEM_RECLAIM, 0)', nhi_c)
        self.assertIn('destroy_workqueue(nhi->dma_hi_wq);', extract(nhi_c, 'nhi_release_dma_wq'))
        self.assertNotIn('queue_work(ring->nhi->dma_wq, &ring->work)', nhi_c)
        self.assertIn('module_param(dma_wq_percpu, bool, 0644);', nhi_c)

    def run_case(self, case):
        result = subprocess.run([str(self.binary), case], capture_output=True, text=True, timeout=30,
                                env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=1:abort_on_error=1'})
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn(f'PASS {case}', result.stdout)


for _case in CASES:
    setattr(NhiRingTests, f'test_{_case}', lambda self, case=_case: self.run_case(case))
