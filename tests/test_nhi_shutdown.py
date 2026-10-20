# SPDX-License-Identifier: GPL-2.0-only
"""Run the actual nhi_shutdown() of the patched nhi.c under ASan/UBSan.

Rings still allocated when the NHI goes away (abandoned DMA owners keep
them on purpose) must have their MSI-X IRQ freed and their completion work
joined before the NHI, its MMIO and its workqueue are freed. The NHI ops,
IRQs and work are logged doubles, not hardware.
"""
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent
CASES = ('abandoned_rings_lose_cpu_paths', 'leaked_rings_lose_cpu_paths', 'clean_shutdown')


def extract(text, name):
    match = re.search(r'^[a-z][^;{}\n]*\b' + name + r'\([^;{}]*?\)\n\{\n.*?^\}\n', text,
                      re.MULTILINE | re.DOTALL)
    if not match:
        raise RuntimeError(f'{name} not found')
    return match.group(0)


class NhiShutdownTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if 'USB4_SOURCE_TREE' not in os.environ:
            raise unittest.SkipTest('USB4_SOURCE_TREE with the patched nhi.c is required')
        nhi_c = (Path(os.environ['USB4_SOURCE_TREE']) / 'drivers/thunderbolt/nhi.c').read_text()
        cls.tmp = tempfile.TemporaryDirectory(prefix='nhi-shutdown-')
        cls.addClassCleanup(cls.tmp.cleanup)
        tmp = Path(cls.tmp.name)
        unit = tmp / 'unit.c'
        unit.write_text('#include "nhi_shutdown_cases.h"\n' +
                        extract(nhi_c, 'nhi_ring_release_cpu') + extract(nhi_c, 'nhi_shutdown'))
        compiler = os.environ.get('CC', 'cc')
        if not shutil.which(compiler):
            raise RuntimeError(f'Required C compiler unavailable: {compiler}')
        cls.binary = tmp / 'nhi_shutdown'
        result = subprocess.run([compiler, '-std=gnu11', '-g', '-O1', '-Wall', '-Wextra', '-Werror',
                                 '-Wno-unused-function', '-fno-omit-frame-pointer', '-fno-pie', '-no-pie',
                                 '-fsanitize=address,undefined', '-I', str(ROOT), str(unit), '-o',
                                 str(cls.binary)], capture_output=True, text=True)
        if result.returncode:
            raise RuntimeError(result.stdout + result.stderr)

    def run_case(self, case):
        result = subprocess.run([str(self.binary), case], capture_output=True, text=True, timeout=30,
                                env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=1:abort_on_error=1'})
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn(f'PASS {case}', result.stdout)


for _case in CASES:
    setattr(NhiShutdownTests, f'test_{_case}', lambda self, case=_case: self.run_case(case))

if __name__ == '__main__':
    unittest.main()
