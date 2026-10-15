#!/usr/bin/env python3
"""Verify the immutable capture, then materialize the audited overlay elsewhere."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile

REPO = Path(__file__).resolve().parents[1]

def materialize(output: Path, repo: Path = REPO) -> None:
    output = output.resolve()
    if output.exists():
        raise ValueError(f"Refusing to overwrite {output}")
    manifest = json.loads((repo / 'provenance/source-manifest.json').read_text())
    for item in manifest['modified_files']:
        path = repo / 'overlay' / item['path']
        if hashlib.sha256(path.read_bytes()).hexdigest() != item['snapshot_sha256']:
            raise ValueError(f"Snapshot hash mismatch: {path}")
    original = repo / 'patches/0001-usb4-current-source.patch'
    if hashlib.sha256(original.read_bytes()).hexdigest() != manifest['combined_patch_sha256']:
        raise ValueError('Original combined patch hash mismatch')
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='.usb4-', dir=output.parent) as tmp:
        tree = Path(tmp) / 'source'
        shutil.copytree(repo / 'overlay', tree)
        for name in (repo / 'patches/audit-series').read_text().splitlines():
            if not name or name.startswith('#'):
                continue
            if Path(name).name != name:
                raise ValueError(f'Invalid patch name: {name}')
            patch = repo / 'patches' / name
            subprocess.run(['git', 'apply', '--check', str(patch)], cwd=tree, check=True)
            subprocess.run(['git', 'apply', '--whitespace=error-all', str(patch)], cwd=tree, check=True)
        shutil.move(str(tree), str(output))
    print(f'Audited overlay: {output}')

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path, help='New, non-existing directory')
    args = parser.parse_args()
    try:
        materialize(args.output)
    except (OSError, ValueError, subprocess.CalledProcessError) as exc:
        parser.exit(1, f'{exc}\n')
