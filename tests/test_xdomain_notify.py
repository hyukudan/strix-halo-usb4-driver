# SPDX-License-Identifier: GPL-2.0-only
"""Run the exact XDomain notification helper with transport/workqueue doubles."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parent
SOURCE=Path(os.environ['USB4_SOURCE_TREE'])
CASES=('post_once','budget','concurrent_update','stale_handshake','removed','no_pingpong','suspend_resume')
class NotifyTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp=tempfile.TemporaryDirectory(prefix='xdomain-notify-')
        cls.addClassCleanup(cls.tmp.cleanup)
        cls.binary=Path(cls.tmp.name)/'notify'
        subprocess.run([os.environ.get('CC','cc'),'-std=gnu11','-g','-O1','-Wall','-Wextra','-Werror',
            '-fno-pie','-no-pie','-fsanitize=address,undefined','-pthread',
            '-I',str(SOURCE/'drivers/thunderbolt'),str(ROOT/'xdomain_notify_tests.c'),'-o',str(cls.binary)],check=True)
    def run_case(self,case):
        result=subprocess.run([str(self.binary),case],capture_output=True,text=True,timeout=10,
            env={**os.environ,'ASAN_OPTIONS':'detect_leaks=1:abort_on_error=1'})
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
for case in CASES:
    setattr(NotifyTests,'test_'+case,lambda self,name=case:self.run_case(name))
