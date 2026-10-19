#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""tools/check_build_log.py: CI gate on diagnostics in this series' files."""
import importlib.util
from pathlib import Path
import unittest

REPO = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('check_build_log', REPO / 'tools/check_build_log.py')
cbl = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cbl)

LINKED = '\n'.join(f'  LD [M]  {m}' for m in cbl.MODULES) + '\n'


class CheckBuildLogTests(unittest.TestCase):
    def test_clean_log_with_all_modules_linked(self):
        log = ('  CC [M]  drivers/thunderbolt/stream.o\n'
               'fs/foo.c:12:3: warning: unused variable x [-Wunused-variable]\n'
               '  MODPOST Module.symvers\n' + LINKED)
        self.assertEqual(cbl.check(log), [])

    def test_diagnostics_in_our_files_fail(self):
        for line in ('drivers/thunderbolt/stream.c:10:5: warning: no previous prototype',
                     '/build/linux/drivers/net/thunderbolt/main.c:3:1: error: oops',
                     "Warning: drivers/thunderbolt/stream.c:198 struct member 'rx_lock' "
                     "not described in 'tbstream_dev'",
                     './include/linux/thunderbolt.h:40: warning: Function parameter not described',
                     'include/uapi/linux/strix_gpu_stream.h:5:1: warning: x',
                     'ERROR: modpost: "tb_ring_poll" [drivers/thunderbolt/thunderbolt_stream.ko] undefined!',
                     'WARNING: modpost: drivers/thunderbolt/thunderbolt: section mismatch'):
            with self.subTest(line=line):
                problems = cbl.check(line + '\n' + LINKED)
                self.assertEqual(len(problems), 1, problems)

    def test_missing_link_fails(self):
        log = '\n'.join(f'  LD [M]  {m}' for m in cbl.MODULES[:2]) + '\n'
        problems = cbl.check(log)
        self.assertEqual(len(problems), 1)
        self.assertIn('thunderbolt_net.ko was not linked', problems[0])

    def test_workflow_uses_full_link_and_gate(self):
        workflow = (REPO / '.github/workflows/audit.yml').read_text()
        self.assertIn('W=1 vmlinux modules', workflow)
        self.assertEqual(workflow.count('tools/check_build_log.py'), 2)
        self.assertIn('--disable USB4_GPU_STREAM', workflow)
        self.assertIn('tools/apply-series.sh', workflow)
        self.assertIn('strix-gpu-queue-visibility', workflow)


if __name__ == '__main__':
    unittest.main(verbosity=2)
