#!/usr/bin/env python3
"""Reconstruction failure/working-directory tests; fixtures contain no driver code."""
import hashlib
import importlib.util
import json
import os
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

    def test_unlisted_overlay_file_is_rejected(self):
        (self.repo / 'overlay/drivers/extra.c').write_bytes(b'unhashed\n')
        with self.assertRaisesRegex(ValueError, r"unexpected \['drivers/extra.c'\]"):
            prepare.materialize(self.output, self.repo)
        self.assertFalse(self.output.exists())

    def test_missing_overlay_file_is_rejected(self):
        self.original.unlink()
        with self.assertRaisesRegex(ValueError, r"missing \['drivers/example.c'\]"):
            prepare.materialize(self.output, self.repo)
        self.assertFalse(self.output.exists())

    def test_overlay_symlinks_are_rejected(self):
        outside = self.repo / 'outside.c'
        outside.write_bytes(b'outside\n')
        for link in (self.repo / 'overlay/drivers/link.c', self.repo / 'overlay/linkdir'):
            with self.subTest(link=link.name):
                os.symlink(outside if link.suffix else self.repo / 'patches', link)
                with self.assertRaisesRegex(ValueError, 'Symlink not allowed'):
                    prepare.materialize(self.output, self.repo)
                self.assertFalse(self.output.exists())
                link.unlink()

    def test_only_inventory_bytes_are_published(self):
        prepare.materialize(self.output, self.repo)
        published = sorted(p.relative_to(self.output).as_posix()
                           for p in self.output.rglob('*') if p.is_file())
        self.assertEqual(published, ['drivers/example.c'])

    def test_audit_series_must_match_series(self):
        (self.repo / 'patches/series').write_text(
            '0001-usb4-current-source.patch\n0002-test.patch\n0003-missing.patch\n')
        with self.assertRaisesRegex(ValueError, 'audit-series must equal'):
            prepare.materialize(self.output, self.repo)
        (self.repo / 'patches/series').write_text('0001-usb4-current-source.patch\n0002-test.patch\n')
        prepare.materialize(self.output, self.repo)

if __name__ == '__main__':
    unittest.main(verbosity=2)
