# SPDX-License-Identifier: GPL-2.0-only
"""Full recovery callback body with network/RTNL doubles; not a network run."""
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parent
CASES=('success','preflight','busy','alloc','reject_restore','failed_reset','failed_stop','failed_restore','empty','unrelated_event')
class NetworkRecoveryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp=tempfile.TemporaryDirectory(prefix='tbnet-recovery-');cls.addClassCleanup(cls.tmp.cleanup)
        source=Path(os.environ['USB4_SOURCE_TREE'])/'drivers/net/thunderbolt/dma_recovery.h'
        code=re.sub(r'^#include[^\n]*\n','',source.read_text(),flags=re.M)
        unit=Path(cls.tmp.name)/'test.c';cls.binary=Path(cls.tmp.name)/'test'
        unit.write_text((ROOT/'tbnet_recovery_test.c').read_text().replace('/* SOURCE_INSERT */',code))
        subprocess.run([os.environ.get('CC','cc'),'-std=gnu11','-O1','-g','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-fsanitize=address,undefined','-fno-pie','-no-pie','-pthread',str(unit),'-o',str(cls.binary)],check=True)
    def run_case(self,name):
        result=subprocess.run([str(self.binary),name],capture_output=True,text=True,timeout=10)
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
for name in CASES:
    setattr(NetworkRecoveryTests,'test_'+name,lambda self,name=name:self.run_case(name))
