#!/usr/bin/env python3
"""Verify the immutable capture, then materialize the audited overlay elsewhere."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

REPO = Path(__file__).resolve().parents[1]


def copy_additional_base(repo: Path, tree: Path) -> None:
    """Add hash-pinned upstream files absent from the immutable capture.

    Never replace a captured file. Only explicit, relative manifest paths are
    copied; symlinks, duplicates, extra/missing files and hash changes fail.
    """
    base = repo / 'base'
    manifest_path = repo / 'provenance/recovery-base.json'
    if not manifest_path.exists():
        if base.exists():
            raise ValueError('Additional base exists without its manifest')
        return
    manifest = json.loads(manifest_path.read_text())
    expected_ref = '72d3fcf802c45d00b300f25b848a93c3a2bd7c7e'
    if manifest['upstream_ref'] != expected_ref:
        raise ValueError('Unexpected additional-base upstream revision')
    files = manifest['files']
    names = [item['path'] for item in files]
    if len(set(names)) != len(names):
        raise ValueError('Duplicate additional-base file')
    actual = {str(p.relative_to(base)) for p in base.rglob('*') if p.is_file() or p.is_symlink()}
    if set(names) != actual:
        raise ValueError('Additional-base inventory mismatch')
    for item in files:
        relative = Path(item['path'])
        if relative.is_absolute() or '..' in relative.parts or not relative.parts:
            raise ValueError('Invalid additional-base path')
        source = base / relative
        if source.is_symlink() or not source.is_file() or base.resolve() not in source.resolve().parents:
            raise ValueError('Invalid additional-base source')
        data = source.read_bytes()
        digest = hashlib.sha1(f'blob {len(data)}\0'.encode() + data).hexdigest()
        if digest != item['git_blob_sha']:
            raise ValueError(f'Additional-base hash mismatch: {relative}')
        target = tree / relative
        if target.exists():
            raise ValueError(f'Additional base would overwrite capture: {relative}')
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)

def materialize(output: Path, repo: Path = REPO) -> None:
    repo = repo.resolve()
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
        copy_additional_base(repo, tree)
        # Never inherit a surrounding repository: git apply otherwise silently
        # skips these paths when output is a subdirectory of the checkout.
        env = os.environ.copy()
        for key in ('GIT_DIR', 'GIT_WORK_TREE', 'GIT_INDEX_FILE'):
            env.pop(key, None)
        env['GIT_CEILING_DIRECTORIES'] = str(tree.parent)
        for name in (repo / 'patches/audit-series').read_text().splitlines():
            if not name or name.startswith('#'):
                continue
            if Path(name).name != name:
                raise ValueError(f'Invalid patch name: {name}')
            patch = repo / 'patches' / name
            subprocess.run(['git', 'apply', '--check', str(patch)], cwd=tree, env=env, check=True)
            subprocess.run(['git', 'apply', '--whitespace=error-all', str(patch)], cwd=tree, env=env, check=True)
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
