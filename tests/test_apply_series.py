#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""tools/apply-series.sh: fail fast on the first patch that does not apply."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[1]
SCRIPT = REPO / 'tools/apply-series.sh'


def patch(path, old, new):
    return (f'diff --git a/{path} b/{path}\n--- a/{path}\n+++ b/{path}\n'
            f'@@ -1 +1 @@\n-{old}\n+{new}\n')


@unittest.skipUnless(shutil.which('sh') and shutil.which('git'), 'needs sh and git')
class ApplySeriesTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        root = Path(self.tmp.name)
        self.repo, self.kernel = root / 'driver', root / 'linux'
        (self.repo / 'tools').mkdir(parents=True)
        (self.repo / 'patches/kernel-backports').mkdir(parents=True)
        shutil.copy(SCRIPT, self.repo / 'tools/apply-series.sh')
        self.kernel.mkdir()
        for name in ('a.c', 'b.c', 'c.c'):
            (self.kernel / name).write_text('old\n')
        self.patches = {'0001-a.patch': patch('a.c', 'old', 'new'),
                        '0002-b.patch': patch('b.c', 'old', 'new'),
                        '0003-c.patch': patch('c.c', 'old', 'new')}
        self.write()

    def write(self, series=None):
        for name, body in self.patches.items():
            (self.repo / 'patches' / name).write_text(body)
        (self.repo / 'patches/series').write_text('\n'.join(series or self.patches) + '\n')

    def run_script(self, *extra):
        return subprocess.run(['sh', str(self.repo / 'tools/apply-series.sh'), *extra,
                               str(self.kernel)], capture_output=True, text=True, timeout=60)

    def contents(self):
        return [(self.kernel / n).read_text().strip() for n in ('a.c', 'b.c', 'c.c')]

    def test_complete_series(self):
        result = self.run_script()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('applied 3 patch(es)', result.stdout)
        self.assertEqual(self.contents(), ['new', 'new', 'new'])

    def test_intermediate_failure_stops_with_error(self):
        # The old README loop returned 0 here because the last patch applied.
        self.patches['0002-b.patch'] = patch('b.c', 'something else', 'new')
        self.write()
        result = self.run_script()
        self.assertEqual(result.returncode, 1)
        self.assertIn('0002-b.patch does not apply; stopped after 1 patch(es)', result.stderr)
        self.assertEqual(self.contents(), ['new', 'old', 'old'])

    def test_unsafe_or_missing_entry_applies_nothing(self):
        for series in (['0001-a.patch', '../evil.patch'], ['0001-a.patch', '0009-missing.patch']):
            with self.subTest(series=series):
                self.write(series)
                result = self.run_script()
                self.assertEqual(result.returncode, 1)
                self.assertEqual(self.contents(), ['old', 'old', 'old'])

    def test_backports_are_opt_in(self):
        self.patches.pop('0003-c.patch')
        self.write()
        (self.repo / 'patches/kernel-backports/0001-c.patch').write_text(patch('c.c', 'old', 'bp'))
        self.assertEqual(self.run_script('--with-backports').returncode, 0)
        self.assertEqual(self.contents(), ['new', 'new', 'bp'])

    def test_experimental_is_opt_in(self):
        self.patches.pop('0003-c.patch')
        self.write()
        experimental = self.repo / 'patches/experimental'
        experimental.mkdir()
        (experimental / '0003-c.patch').write_text(patch('c.c', 'old', 'exp'))
        (experimental / 'series').write_text('# on top of patches/series\n0003-c.patch\n')
        self.assertEqual(self.run_script().returncode, 0)
        self.assertEqual(self.contents(), ['new', 'new', 'old'])
        for name in ('a.c', 'b.c', 'c.c'):
            (self.kernel / name).write_text('old\n')
        result = self.run_script('--with-experimental')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('applied 3 patch(es)', result.stdout)
        self.assertEqual(self.contents(), ['new', 'new', 'exp'])
        for name in ('a.c', 'b.c', 'c.c'):
            (self.kernel / name).write_text('old\n')
        (experimental / 'series').write_text('0003-c.patch\n../0001-a.patch\n')
        self.assertEqual(self.run_script('--with-experimental').returncode, 1)
        self.assertEqual(self.contents(), ['old', 'old', 'old'])

    def test_usage(self):
        result = subprocess.run(['sh', str(self.repo / 'tools/apply-series.sh')],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 2)

    def test_docs_use_the_canonical_procedure(self):
        for doc in ('README.md', 'BUILD.md'):
            text = (REPO / doc).read_text()
            with self.subTest(doc=doc):
                self.assertIn('tools/apply-series.sh', text)
                self.assertNotIn('git apply --check "$OLDPWD/patches/$p" && git apply', text)


if __name__ == '__main__':
    unittest.main(verbosity=2)
