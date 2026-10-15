#!/usr/bin/env python3
"""Reconstruction failure/working-directory tests; fixtures contain no driver code."""
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('prepare', ROOT / 'tools/prepare_overlay.py')
prepare = importlib.util.module_from_spec(spec)
spec.loader.exec_module(prepare)

class ReconstructionTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.repo = Path(self.tmp.name)
        subprocess.run(['git', 'init', '-q', str(self.repo)], check=True)
        for name in ('overlay/drivers', 'provenance', 'patches'):
            (self.repo / name).mkdir(parents=True)
        self.original = self.repo / 'overlay/drivers/example.c'
        self.original.write_bytes(b'old\n')
        base = self.repo / 'patches/0001-usb4-current-source.patch'
        base.write_bytes(b'fixture-only base patch\n')
        manifest = dict(modified_files=[dict(path='drivers/example.c',
            snapshot_sha256=hashlib.sha256(self.original.read_bytes()).hexdigest())],
            combined_patch_sha256=hashlib.sha256(base.read_bytes()).hexdigest())
        (self.repo / 'provenance/source-manifest.json').write_text(json.dumps(manifest))
        (self.repo / 'patches/audit-series').write_text('0002-test.patch\n')
        (self.repo / 'patches/0002-test.patch').write_text(
            'diff --git a/drivers/example.c b/drivers/example.c\n'
            '--- a/drivers/example.c\n+++ b/drivers/example.c\n'
            '@@ -1 +1 @@\n-old\n+new\n')
        self.output = self.repo / 'audited-overlay'

    def test_patch_applies_inside_surrounding_git_checkout(self):
        prepare.materialize(self.output, self.repo)
        self.assertEqual((self.output / 'drivers/example.c').read_bytes(), b'new\n')
        self.assertEqual(self.original.read_bytes(), b'old\n')
        with self.assertRaises(ValueError):
            prepare.materialize(self.output, self.repo)

    def test_modified_snapshot_is_rejected(self):
        self.original.write_bytes(b'tampered\n')
        with self.assertRaises(ValueError):
            prepare.materialize(self.output, self.repo)
        self.assertFalse(self.output.exists())

    def test_modified_base_patch_is_rejected(self):
        (self.repo / 'patches/0001-usb4-current-source.patch').write_bytes(b'tampered\n')
        with self.assertRaises(ValueError):
            prepare.materialize(self.output, self.repo)
        self.assertFalse(self.output.exists())

    def test_bad_patch_does_not_publish_partial_output(self):
        (self.repo / 'patches/0002-test.patch').write_text('not a patch\n')
        with self.assertRaises(subprocess.CalledProcessError):
            prepare.materialize(self.output, self.repo)
        self.assertFalse(self.output.exists())
        self.assertEqual(self.original.read_bytes(), b'old\n')

if __name__ == '__main__':
    unittest.main(verbosity=2)
