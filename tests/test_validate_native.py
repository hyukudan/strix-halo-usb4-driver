#!/usr/bin/env python3
"""tools/validate_native.py, update_validation_manifest.py and check_kconfig.py.

Fixture repositories contain no driver code. The repository tests check that
the shipped manifest matches the shipped series and, when USB4_SOURCE_TREE is
set, the reconstructed source.
"""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / 'tools'))


def load(name):
    spec = importlib.util.spec_from_file_location(name, REPO / 'tools' / f'{name}.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


vn = load('validate_native')
um = load('update_validation_manifest')
ck = load('check_kconfig')

DIFF = ('diff --git a/{0} b/{0}\n--- a/{0}\n+++ b/{0}\n@@ -1 +1 @@\n-old\n+new\n')
NEW = ('diff --git a/{0} b/{0}\nnew file mode 100644\n--- /dev/null\n+++ b/{0}\n@@ -0,0 +1 @@\n+x\n')
GONE = ('diff --git a/{0} b/{0}\ndeleted file mode 100644\n--- a/{0}\n+++ /dev/null\n@@ -1 +0,0 @@\n-x\n')


class FixtureRepo:
    def __init__(self, root, count=3):
        self.root = Path(root)
        (self.root / 'patches').mkdir(parents=True)
        (self.root / 'provenance').mkdir()
        names = [vn.BASE_PATCH] + [f'{i:04d}-change-{i}.patch' for i in range(2, count + 1)]
        bodies = [DIFF.format(vn.DRIVER_UAPI)] + [DIFF.format(f'drivers/f{i}.c')
                                                  for i in range(2, count + 1)]
        for name, body in zip(names, bodies):
            (self.root / 'patches' / name).write_text(body)
        self.names = names
        self.write_series(names)

    def write_series(self, names, audit=None):
        (self.root / 'patches/series').write_text('\n'.join(names) + '\n')
        (self.root / 'patches/audit-series').write_text(
            '\n'.join(names[1:] if audit is None else audit) + '\n')


class SeriesTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.repo = FixtureRepo(self.tmp.name)

    def test_any_length_contiguous_series_is_accepted(self):
        for count in (1, 9, 19, 20):
            with self.subTest(count=count), tempfile.TemporaryDirectory() as tmp:
                repo = FixtureRepo(tmp, count)
                self.assertEqual([p.name for p in vn.series(repo.root)], repo.names)

    def test_gap_or_reordering_is_rejected(self):
        self.repo.write_series([self.repo.names[0], self.repo.names[2], self.repo.names[1]])
        with self.assertRaisesRegex(ValueError, 'not contiguous'):
            vn.series(self.repo.root)

    def test_series_must_start_with_base(self):
        self.repo.write_series(self.repo.names[1:])
        with self.assertRaisesRegex(ValueError, 'must start with'):
            vn.series(self.repo.root)

    def test_unlisted_patch_is_rejected(self):
        (self.repo.root / 'patches/0004-stray.patch').write_text('x')
        with self.assertRaisesRegex(ValueError, 'Unlisted patch.*0004-stray'):
            vn.series(self.repo.root)

    def test_audit_series_must_match(self):
        self.repo.write_series(self.repo.names, audit=self.repo.names[1:2])
        with self.assertRaisesRegex(ValueError, 'audit-series'):
            vn.series(self.repo.root)

    def test_unsafe_or_symlinked_entries_are_rejected(self):
        self.repo.write_series(self.repo.names + ['../0004-x.patch'])
        with self.assertRaisesRegex(ValueError, 'Unsafe'):
            vn.series(self.repo.root)
        link = self.repo.root / 'patches/0004-link.patch'
        os.symlink(self.repo.root / 'patches' / self.repo.names[1], link)
        self.repo.write_series(self.repo.names + ['0004-link.patch'])
        with self.assertRaisesRegex(ValueError, 'symlink'):
            vn.series(self.repo.root)

    def test_touched_files_follow_creations_and_deletions(self):
        patches = vn.series(self.repo.root)
        extra = self.repo.root / 'patches/0004-new.patch'
        extra.write_text(NEW.format('drivers/new.c') + GONE.format('drivers/f2.c'))
        self.assertEqual(vn.touched_files(patches + [extra]),
                         {vn.DRIVER_UAPI, 'drivers/f3.c', 'drivers/new.c'})


class ManifestTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        root = Path(self.tmp.name)
        self.repo = FixtureRepo(root / 'driver')
        self.source = root / 'source'
        for name in (vn.DRIVER_UAPI, 'drivers/f2.c', 'drivers/f3.c'):
            (self.source / name).parent.mkdir(parents=True, exist_ok=True)
            (self.source / name).write_text(f'new {name}\n')
        self.engine = root / 'engine'
        for rel in vn.ENGINE_FILES:
            (self.engine / rel).parent.mkdir(parents=True, exist_ok=True)
            (self.engine / rel).write_text(f'engine {rel}\n')
        (self.engine / vn.ENGINE_UAPI).write_bytes((self.source / vn.DRIVER_UAPI).read_bytes())

    def generate(self):
        self.assertEqual(um.main(['--driver-repo', str(self.repo.root), '--source', str(self.source),
                                  '--engine-repo', str(self.engine), '--write']), 0)
        return vn.load_manifest(self.repo.root, vn.series(self.repo.root))

    def test_generated_manifest_covers_every_touched_file(self):
        manifest = self.generate()
        self.assertEqual(sorted(manifest['materialized_files']),
                         sorted([vn.DRIVER_UAPI, 'drivers/f2.c', 'drivers/f3.c']))
        self.assertEqual(manifest['patch_count'], 3)
        vn.verify_materialized_kernel(self.source, manifest)
        vn.verify_engine(self.engine, manifest)

    def test_manifest_is_read_from_driver_repo_not_script_root(self):
        self.generate()
        other = FixtureRepo(Path(self.tmp.name) / 'other', 4)
        with self.assertRaises(FileNotFoundError):
            vn.load_manifest(other.root, vn.series(other.root))
        manifest = vn.load_manifest(self.repo.root, vn.series(self.repo.root))
        self.assertEqual(manifest['last_patch'], self.repo.names[-1])

    def test_series_change_invalidates_manifest(self):
        self.generate()
        (self.repo.root / 'patches' / self.repo.names[-1]).write_text(DIFF.format('drivers/f9.c'))
        with self.assertRaisesRegex(ValueError, 'different patch series'):
            vn.load_manifest(self.repo.root, vn.series(self.repo.root))

    def test_stale_materialized_hash_is_rejected(self):
        manifest = self.generate()
        (self.source / 'drivers/f3.c').write_text('tampered\n')
        with self.assertRaisesRegex(ValueError, 'differs from delivered implementation: drivers/f3.c'):
            vn.verify_materialized_kernel(self.source, manifest)

    def test_engine_uapi_drift_is_refused(self):
        (self.engine / vn.ENGINE_UAPI).write_text('older engine header\n')
        self.assertEqual(um.main(['--driver-repo', str(self.repo.root), '--source', str(self.source),
                                  '--engine-repo', str(self.engine)]), 1)
        self.assertFalse((self.repo.root / vn.MANIFEST).exists())
        (self.engine / vn.ENGINE_UAPI).write_bytes((self.source / vn.DRIVER_UAPI).read_bytes())
        manifest = self.generate()
        (self.engine / vn.ENGINE_UAPI).write_text('older engine header\n')
        with self.assertRaisesRegex(ValueError, 'UAPI drift'):
            vn.verify_engine(self.engine, manifest)

    def test_old_schema_requests_regeneration(self):
        (self.repo.root / vn.MANIFEST).write_text(json.dumps({'materialized_files': {}}))
        with self.assertRaisesRegex(ValueError, 'regenerate'):
            vn.load_manifest(self.repo.root, vn.series(self.repo.root))


class RepositoryTests(unittest.TestCase):
    """The shipped series, manifest and fragment are consistent with each other."""

    def test_shipped_manifest_matches_shipped_series(self):
        patches = vn.series(REPO)
        manifest = vn.load_manifest(REPO, patches)
        self.assertEqual(set(manifest['materialized_files']), vn.touched_files(patches))

    @unittest.skipUnless(os.environ.get('USB4_SOURCE_TREE'), 'needs a reconstructed source')
    def test_shipped_manifest_matches_reconstruction(self):
        manifest = vn.load_manifest(REPO, vn.series(REPO))
        vn.verify_materialized_kernel(Path(os.environ['USB4_SOURCE_TREE']), manifest)

    def test_offline_config_reproduces_the_fragment(self):
        fragment = (REPO / 'config/usb4-gpu.fragment').read_text()
        on, off = vn.config_options(fragment, 'on'), vn.config_options(fragment, 'off')
        for opt in (['-e', 'CONFIGFS_FS'], ['-m', 'USB4_CONFIGFS'], ['-m', 'USB4_STREAM'],
                    ['-m', 'USB4_NET'], ['-e', 'USB4_GPU_STREAM']):
            self.assertIn(' '.join(opt), ' '.join(on))
        self.assertIn('-d USB4_GPU_STREAM', ' '.join(off))
        self.assertEqual(on[-3:], ['--set-str', 'LOCALVERSION', vn.OFFLINE_LOCALVERSION])
        self.assertEqual(ck.check(_config(ck.parse(fragment), LOCALVERSION='"%s"' % vn.OFFLINE_LOCALVERSION),
                                  fragment), [])


def _config(values, **override):
    values = dict(values)
    values.update({f'CONFIG_{k}': v for k, v in override.items()})
    return '\n'.join(f'# {k} is not set' if v == 'n' else f'{k}={v}' for k, v in values.items()) + '\n'


class KconfigCheckTests(unittest.TestCase):
    FRAGMENT = (REPO / 'config/usb4-gpu.fragment').read_text()

    def good(self, **override):
        values = ck.parse(self.FRAGMENT)
        values['CONFIG_LOCALVERSION'] = '"-strix-gpu-bosgame-20261020a"'
        return _config(values, **override)

    def test_fragment_requires_configfs_and_placeholder_identity(self):
        values = ck.parse(self.FRAGMENT)
        self.assertEqual(values['CONFIG_CONFIGFS_FS'], 'y')
        self.assertEqual(values['CONFIG_LOCALVERSION'], f'"{ck.PLACEHOLDER}"')
        self.assertEqual(values['CONFIG_LOCALVERSION_AUTO'], 'n')

    def test_complete_config_passes(self):
        self.assertEqual(ck.check(self.good(), self.FRAGMENT), [])

    def test_dropped_dependency_is_reported(self):
        problems = ck.check(self.good(CONFIGFS_FS='n', USB4_CONFIGFS='n', USB4_STREAM='n',
                                      USB4_GPU_STREAM='n'), self.FRAGMENT)
        self.assertEqual(len(problems), 4)
        self.assertTrue(all('final .config has n' in p for p in problems))

    def test_gpu_off_build(self):
        self.assertEqual(ck.check(self.good(USB4_GPU_STREAM='n'), self.FRAGMENT, gpu=False), [])
        self.assertEqual(len(ck.check(self.good(), self.FRAGMENT, gpu=False)), 1)

    def test_identity_rules(self):
        cases = {f'"{ck.PLACEHOLDER}"': 'placeholder',
                 '"-strix-gpu-bosgame-20261014"': 'already used',
                 '"-custom"': 'must be',
                 '""': 'must be',
                 '"-strix-gpu-bosgame-a b"': 'must be'}
        for value, message in cases.items():
            with self.subTest(value=value):
                problems = ck.check(self.good(LOCALVERSION=value), self.FRAGMENT)
                self.assertEqual(len(problems), 1, problems)
                self.assertIn(message, problems[0])
        self.assertIn('already used', ck.check(self.good(), self.FRAGMENT,
                                               forbid=['-strix-gpu-bosgame-20261020a'])[0])

    def test_existing_module_directory_is_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            release = '7.3.0-rc5-strix-gpu-bosgame-20261020a'
            self.assertEqual(ck.check(self.good(), self.FRAGMENT, kernelrelease=release,
                                      modules_root=tmp), [])
            (Path(tmp) / release).mkdir()
            self.assertIn('already exists', ck.check(self.good(), self.FRAGMENT,
                                                     kernelrelease=release, modules_root=tmp)[0])
            self.assertIn('does not end with',
                          ck.check(self.good(), self.FRAGMENT, kernelrelease='7.3.0-rc5+',
                                   modules_root=tmp)[0])

    def test_cli_exit_status(self):
        with tempfile.TemporaryDirectory() as tmp:
            cfg = Path(tmp) / '.config'
            cfg.write_text(self.good())
            self.assertEqual(ck.main([str(cfg)]), 0)
            cfg.write_text(self.good(CONFIGFS_FS='n'))
            self.assertEqual(ck.main([str(cfg)]), 1)


if __name__ == '__main__':
    unittest.main(verbosity=2)
