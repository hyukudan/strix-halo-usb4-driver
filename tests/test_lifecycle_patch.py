# SPDX-License-Identifier: GPL-2.0-only
"""Apply/reverse the complete new patch on full GPU sources + marked contexts.

NOT a complete upstream reconstruction or compilation. Source-context fixtures
only check hunks/wiring; the operator's APPLY runs the real full-series rebuild.
"""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parent
BUNDLE=ROOT.parents[1]
PATCH=BUNDLE/'patches/0009-domain-retirement-and-cooperative-controller-recovery.patch'
SOURCE=Path(os.environ.get('USB4_SOURCE_TREE',BUNDLE/'driver-source'))
BASE=BUNDLE/'baseline-source'
class LifecyclePatchTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not PATCH.is_file() or not BASE.is_dir():
            # Repository runs use the separate full-reconstruction tests and
            # hashes; this bundled test explicitly needs the retained baseline.
            raise unittest.SkipTest('bundle-only source-context/roundtrip test')
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory();self.addCleanup(self.tmp.cleanup)
        self.root=Path(self.tmp.name)/'tree';shutil.copytree(BASE,self.root)
        self.expected={}
        fragments=json.loads((ROOT/'completion_contexts.json').read_text())['fragments']
        for name in sorted({f['path'] for f in fragments}):
            group=sorted((f for f in fragments if f['path']==name),key=lambda f:f['line'])
            lines=[]
            for f in group:
                self.assertLessEqual(len(lines),f['line']-1)
                lines+=['/* TEST CONTEXT FILLER, NOT UPSTREAM */\n']*(f['line']-1-len(lines))
                lines+=f['before'].splitlines(True)
            data=''.join(lines)
            target=self.root/name;target.parent.mkdir(parents=True,exist_ok=True);target.write_text(data)
            for f in group:
                self.assertEqual(data.count(f['before']),1)
                data=data.replace(f['before'],f['after'],1)
            self.expected[name]=data
        self.before={p.relative_to(self.root).as_posix():p.read_bytes() for p in self.root.rglob('*') if p.is_file()}
        self.env=os.environ.copy()
        for key in ('GIT_DIR','GIT_WORK_TREE','GIT_INDEX_FILE'):self.env.pop(key,None)
        self.env['GIT_CEILING_DIRECTORIES']=str(self.root.parent)
    def git(self,*args,check=True):
        return subprocess.run(['git','-c','core.hooksPath=/dev/null',*args],cwd=self.root,env=self.env,capture_output=True,text=True,check=check)
    def test_complete_patch_and_reverse_roundtrip(self):
        self.git('apply','--check',str(PATCH))
        self.git('apply','--whitespace=error-all',str(PATCH))
        for p in SOURCE.rglob('*'):
            if p.is_file():self.assertEqual((self.root/p.relative_to(SOURCE)).read_bytes(),p.read_bytes(),str(p))
        for name,data in self.expected.items():self.assertEqual((self.root/name).read_text(),data)
        self.git('apply','--reverse','--check',str(PATCH))
        self.git('apply','--reverse',str(PATCH))
        after={p.relative_to(self.root).as_posix():p.read_bytes() for p in self.root.rglob('*') if p.is_file()}
        self.assertEqual(after,self.before)
    def test_missing_cm_wiring_is_rejected(self):
        p=self.root/'drivers/thunderbolt/domain.c';p.write_text(p.read_text().replace('flush_workqueue(tb->wq);','flush_different_queue(tb->wq);'))
        before={p.relative_to(self.root).as_posix():p.read_bytes() for p in self.root.rglob('*') if p.is_file()}
        self.assertNotEqual(self.git('apply','--check',str(PATCH),check=False).returncode,0)
        after={p.relative_to(self.root).as_posix():p.read_bytes() for p in self.root.rglob('*') if p.is_file()}
        self.assertEqual(before,after)
