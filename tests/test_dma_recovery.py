# SPDX-License-Identifier: GPL-2.0-only
"""Execute the real reset implementation with mocked PCI/MMIO/PM, not hardware."""
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = Path(os.environ.get('USB4_SOURCE_TREE', ROOT / 'audited-overlay'))
CASES = ('admission', 'reset_success', 'unsupported', 'reset_failure', 'control_failure', 'inventory_veto', 'inventory_query', 'group_inventory', 'group_reset', 'core_notifier', 'teardown_revoke',
         'teardown_reset_idle', 'teardown_reset_other_stream', 'teardown_reset_waits_for_owner',
         'teardown_reset_lock_busy', 'teardown_reset_tunnel_veto', 'teardown_reset_gated',
         'teardown_reset_failure', 'teardown_reset_budget_restarts',
         'teardown_reset_pending_until_owner_leaves', 'teardown_reset_rechecks_before_reset',
         'teardown_reset_pending_at_removal', 'teardown_reset_pending_across_suspend')

class DmaRecoveryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix='dma-recovery-')
        cls.addClassCleanup(cls.tmp.cleanup)
        tmp = Path(cls.tmp.name)
        code = (SOURCE / 'drivers/thunderbolt/dma_recovery.c').read_text()
        code = re.sub(r'^#include[^\n]*\n', '', code, flags=re.MULTILINE)
        unit = tmp / 'test.c'
        unit.write_text('#include "dma_recovery_harness.h"\n' + code +
                        '\n#include "dma_recovery_cases.h"\n')
        cls.binary = tmp / 'test'
        result = subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-g', '-O1',
            '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined', '-pthread', '-fno-pie', '-no-pie',
            '-I', str(ROOT / 'tests'), str(unit), '-o', str(cls.binary)],
            capture_output=True, text=True)
        if result.returncode:
            raise RuntimeError(result.stdout + result.stderr)

    def run_case(self, name):
        result = subprocess.run([str(self.binary), name], capture_output=True,
            text=True, timeout=10, env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=1:abort_on_error=1'})
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('PASS ' + name, result.stdout)

for case in CASES:
    setattr(DmaRecoveryTests, 'test_' + case, lambda self, name=case: self.run_case(name))
