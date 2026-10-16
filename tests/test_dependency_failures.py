# SPDX-License-Identifier: GPL-2.0-only
"""Failed external fences: real driver functions, kernel API test doubles."""
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

class DependencyTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix='sgu-dependency-')
        cls.addClassCleanup(cls.tmp.cleanup)
        work = Path(cls.tmp.name)
        source = Path(os.environ.get('USB4_SOURCE_TREE', work / 'source'))
        if 'USB4_SOURCE_TREE' not in os.environ:
            subprocess.run(['python3', str(ROOT / 'tools/prepare_overlay.py'), str(source)], check=True)
        code = (source / 'drivers/thunderbolt/gpu_stream.c').read_text()
        code = re.sub(r'^#include[^\n]*\n', '', code, flags=re.MULTILINE)
        unit = work / 'test.c'
        unit.write_text('#include "sgu_test_kernel.h"\n#include "sgu_dependency_status.h"\n#include "gpu_stream_logic.h"\n' + code +
            '\n#define main original_cases_main\n#include "sgu_queue_cases.h"\n#undef main\n#include "sgu_dependency_cases.h"\n')
        cls.binary = work / 'test'
        subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-g', '-O1', '-Wall', '-Wextra', '-Werror',
            '-Wno-unused-function', '-Wno-unused-parameter', '-Wno-unused-const-variable',
            '-fno-omit-frame-pointer', '-fsanitize=address,undefined', '-pthread',
            '-I', str(ROOT / 'tests'), '-I', str(source / 'drivers/thunderbolt'),
            '-I', str(source / 'include/uapi'), str(unit), '-o', str(cls.binary)], check=True)

    def run_case(self, case):
        result = subprocess.run([str(self.binary), case], capture_output=True, text=True, timeout=20,
            env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=1:abort_on_error=1'})
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('PASS ' + case, result.stdout)

    def test_signaled_error(self): self.run_case('signaled_error')
    def test_wait_error_pruned(self): self.run_case('wait_error_pruned')
    def test_wait_stale(self): self.run_case('wait_stale')

    def test_nonblocking_pruned(self): self.run_case('nonblocking_pruned')
