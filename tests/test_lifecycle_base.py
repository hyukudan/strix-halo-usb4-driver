# SPDX-License-Identifier: GPL-2.0-only
"""Validate external-base mechanics with explicit synthetic file bytes."""
import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('lifecycle_prepare',ROOT/'tools/prepare_overlay.py')
module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
REF='72d3fcf802c45d00b300f25b848a93c3a2bd7c7e'
class LifecycleBaseTests(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory();self.addCleanup(self.tmp.cleanup)
        root=Path(self.tmp.name);self.repo=root/'repo';self.tree=root/'tree';self.kernel=root/'kernel'
        for d in (self.repo/'provenance',self.tree,self.kernel/'drivers/thunderbolt'):d.mkdir(parents=True)
        self.entries=[]
        for name in ('tb.c','tb.h'):
            data=('/* SYNTHETIC '+name+' */\n').encode()
            path='drivers/thunderbolt/'+name;(self.kernel/path).write_bytes(data)
            self.entries.append({'path':path,'git_blob_sha':hashlib.sha1(f'blob {len(data)}\0'.encode()+data).hexdigest()})
        self.write()
    def write(self,ref=REF):
        (self.repo/'provenance/lifecycle-base.json').write_text(json.dumps({'upstream_ref':ref,'files':self.entries}))
    def copy(self):module.copy_external_base(self.repo,self.tree,self.kernel)
    def test_exact_copy_without_modifying_inputs(self):
        self.copy()
        for e in self.entries:self.assertEqual((self.tree/e['path']).read_bytes(),(self.kernel/e['path']).read_bytes())
    def test_missing_source_not_silently_skipped(self):
        with self.assertRaisesRegex(ValueError,'kernel-source'):module.copy_external_base(self.repo,self.tree,None)
    def test_all_or_nothing_hash_validation(self):
        (self.kernel/self.entries[1]['path']).write_text('changed')
        with self.assertRaisesRegex(ValueError,'hash mismatch'):self.copy()
        self.assertEqual(list(self.tree.iterdir()),[])
    def test_existing_file_not_overwritten(self):
        p=self.tree/self.entries[0]['path'];p.parent.mkdir(parents=True);p.write_text('captured')
        with self.assertRaises(ValueError):self.copy()
        self.assertEqual(p.read_text(),'captured')
    def test_duplicate_traversal_and_revision_rejected(self):
        baseline=list(self.entries)
        for entries in (baseline+baseline[:1],[{'path':'../escape','git_blob_sha':'0'*40}],[{'path':'/etc/passwd','git_blob_sha':'0'*40}]):
            self.entries=entries;self.write()
            with self.assertRaises(ValueError):self.copy()
        self.entries=baseline;self.write('wrong')
        with self.assertRaises(ValueError):self.copy()
    def test_source_symlinks_rejected(self):
        p=self.kernel/self.entries[0]['path'];p.unlink();p.symlink_to(self.kernel/self.entries[1]['path'])
        with self.assertRaisesRegex(ValueError,'Symlink'):self.copy()
    def test_legacy_repo_without_registry_unchanged(self):
        (self.repo/'provenance/lifecycle-base.json').unlink()
        module.copy_external_base(self.repo,self.tree,None)
        self.assertEqual(list(self.tree.iterdir()),[])
