# SPDX-License-Identifier: GPL-2.0-only
"""Run the actual tb_stop() and DMA inventory query under AddressSanitizer.

Regression test for the use-after-free on domain removal with an armed
gpu_direct session: tb_stop() must unlink every tunnel it frees, because
domain retirement queries tcm->tunnel_list afterwards. Only the function
body comes from the patched tb.c; routers, ports and tunnels are doubles.
"""
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent
CASES = ("stop_then_query", "stale_after_stop")


def extract_function(text, signature):
    match = re.search(r'^' + re.escape(signature) + r'\n\{\n.*?^\}\n', text, re.MULTILINE | re.DOTALL)
    if not match:
        raise RuntimeError(f'{signature} not found')
    return match.group(0)


class TbStopInventoryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if 'USB4_SOURCE_TREE' not in os.environ:
            raise unittest.SkipTest('USB4_SOURCE_TREE with the patched tb.c is required')
        source = Path(os.environ['USB4_SOURCE_TREE']) / 'drivers/thunderbolt'
        tb_c = source / 'tb.c'
        if not tb_c.exists():
            raise unittest.SkipTest('tb.c is not part of this source tree')
        cls.tmp = tempfile.TemporaryDirectory(prefix='tb-stop-')
        cls.addClassCleanup(cls.tmp.cleanup)
        tmp = Path(cls.tmp.name)
        body = extract_function(tb_c.read_text(), 'static void tb_stop(struct tb *tb)')
        unit = tmp / 'unit.c'
        unit.write_text('#include "tb_stop_harness.h"\n' + body +
                        '\n#include "dma_path_query.h"\n#include "tb_stop_cases.h"\n')
        compiler = os.environ.get('CC', 'cc')
        if not shutil.which(compiler):
            raise RuntimeError(f'Required C compiler unavailable: {compiler}')
        cls.binary = tmp / 'tb_stop'
        result = subprocess.run([compiler, '-std=gnu11', '-g', '-O1', '-Wall', '-Wextra', '-Werror',
                                 '-Wno-unused-function', '-fno-omit-frame-pointer', '-fno-pie', '-no-pie',
                                 '-fsanitize=address,undefined', '-I', str(ROOT), '-I', str(source),
                                 str(unit), '-o', str(cls.binary)], capture_output=True, text=True)
        if result.returncode:
            raise RuntimeError(result.stdout + result.stderr)

    def run_case(self, case):
        result = subprocess.run([str(self.binary), case], capture_output=True, text=True, timeout=10,
                                env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=1:abort_on_error=1'})
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn(f'PASS {case}', result.stdout)


for _case in CASES:
    setattr(TbStopInventoryTests, f'test_{_case}', lambda self, case=_case: self.run_case(case))

if __name__ == '__main__':
    unittest.main()
