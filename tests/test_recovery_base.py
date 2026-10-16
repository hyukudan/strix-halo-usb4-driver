# SPDX-License-Identifier: GPL-2.0-only
"""Additional upstream base integrity; test fixtures contain no kernel code."""
import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('recovery_prepare', ROOT / 'tools/prepare_overlay.py')
prepare = importlib.util.module_from_spec(spec)
spec.loader.exec_module(prepare)

class RecoveryBaseTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.repo = Path(self.tmp.name)
        self.tree = self.repo / 'out'
        self.source = self.repo / 'base/include/linux/example.h'
        self.source.parent.mkdir(parents=True)
        self.source.write_bytes(b'example\n')
        (self.repo / 'provenance').mkdir()
        self.manifest = self.repo / 'provenance/recovery-base.json'
        content = self.source.read_bytes()
        self.data = dict(upstream_ref='72d3fcf802c45d00b300f25b848a93c3a2bd7c7e', files=[dict(
            path='include/linux/example.h', git_blob_sha=hashlib.sha1(f'blob {len(content)}\0'.encode()+content).hexdigest())])
        self.save()

    def save(self):
        self.manifest.write_text(json.dumps(self.data))

    def test_pinned_bytes_are_copied(self):
        prepare.copy_additional_base(self.repo, self.tree)
        self.assertEqual((self.tree / 'include/linux/example.h').read_bytes(), b'example\n')
        with self.assertRaises(ValueError):
            prepare.copy_additional_base(self.repo, self.tree)

    def test_tamper_and_missing_manifest_rejected(self):
        self.source.write_bytes(b'changed\n')
        with self.assertRaises(ValueError):
            prepare.copy_additional_base(self.repo, self.tree)
        self.manifest.unlink()
        with self.assertRaises(ValueError):
            prepare.copy_additional_base(self.repo, self.tree)

    def test_path_traversal_and_extra_file_rejected(self):
        self.data['files'][0]['path'] = '../example.h'
        self.save()
        with self.assertRaises(ValueError):
            prepare.copy_additional_base(self.repo, self.tree)
        self.data['files'][0]['path'] = 'include/linux/example.h'
        self.save()
        (self.source.parent / 'extra.h').write_text('unexpected')
        with self.assertRaises(ValueError):
            prepare.copy_additional_base(self.repo, self.tree)

    def test_symlink_rejected(self):
        self.source.unlink()
        external = self.repo / 'external.h'
        external.write_bytes(b'example\n')
        self.source.symlink_to(external)
        with self.assertRaises(ValueError):
            prepare.copy_additional_base(self.repo, self.tree)

    def test_checked_in_base_is_exact(self):
        with tempfile.TemporaryDirectory() as tmp:
            prepare.copy_additional_base(ROOT, Path(tmp))
            self.assertEqual((Path(tmp) / 'include/linux/thunderbolt.h').read_bytes(),
                             (ROOT / 'base/include/linux/thunderbolt.h').read_bytes())
