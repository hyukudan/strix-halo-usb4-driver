# SPDX-License-Identifier: GPL-2.0-only
"""Run the exact actual CM DMA inventory callback with list/path doubles."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parent
SOURCE=Path(os.environ['USB4_SOURCE_TREE'])
CASES=('filters','list')
class PathQueryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp=tempfile.TemporaryDirectory(prefix='dma-path-query-')
        cls.addClassCleanup(cls.tmp.cleanup)
        cls.binary=Path(cls.tmp.name)/'notify'
        subprocess.run([os.environ.get('CC','cc'),'-std=gnu11','-g','-O1','-Wall','-Wextra','-Werror',
            '-fno-pie','-no-pie','-fsanitize=address,undefined','-pthread',
            '-I',str(SOURCE/'drivers/thunderbolt'),str(ROOT/'dma_path_query_tests.c'),'-o',str(cls.binary)],check=True)
    def run_case(self,case):
        result=subprocess.run([str(self.binary),case],capture_output=True,text=True,timeout=10,
            env={**os.environ,'ASAN_OPTIONS':'detect_leaks=1:abort_on_error=1'})
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
for case in CASES:
    setattr(PathQueryTests,'test_'+case,lambda self,name=case:self.run_case(name))
