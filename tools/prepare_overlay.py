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


def copy_external_base(repo: Path, tree: Path, kernel_source: Path | None) -> None:
    """Copy only manifest-named, hash-pinned upstream files; no network access.

    The connection-manager files were not part of the original capture. Require
    their authentic bytes instead of reconstructing a pretend complete source
    from test fragments or silently omitting the lifecycle integration.
    """
    registry = repo / 'provenance/lifecycle-base.json'
    if not registry.exists():
        return
    manifest = json.loads(registry.read_text())
    if not isinstance(manifest, dict) or manifest.get('upstream_ref') != '72d3fcf802c45d00b300f25b848a93c3a2bd7c7e':
        raise ValueError('Unexpected lifecycle-base revision')
    entries = manifest.get('files')
    if not isinstance(entries, list) or not entries or len(entries) > 64:
        raise ValueError('Invalid lifecycle-base inventory')
    if kernel_source is None:
        raise ValueError('Lifecycle patch requires --kernel-source PATH (or USB4_KERNEL_SOURCE) '
                         'with the pinned upstream tb.c, tb.h and USB4NET main.c; no source is downloaded')
    base = Path(kernel_source).absolute()
    if base.is_symlink() or not base.is_dir():
        raise ValueError('Invalid/symlinked kernel-source directory')
    ready, seen = [], set()
    for entry in entries:
        if not isinstance(entry, dict) or not isinstance(entry.get('path'), str):
            raise ValueError('Invalid lifecycle-base entry')
        name = entry['path']
        relative = Path(name)
        if (relative.is_absolute() or '..' in relative.parts or not relative.parts or
                relative.as_posix() != name or name in seen):
            raise ValueError('Invalid/duplicate lifecycle-base path')
        seen.add(name)
        source = base
        for part in relative.parts:
            source /= part
            if source.is_symlink():
                raise ValueError(f'Symlink in lifecycle-base source: {relative}')
        if not source.is_file() or base.resolve() not in source.resolve().parents:
            raise ValueError(f'Missing lifecycle base: {relative}')
        if source.stat().st_size > 16 * 1024 * 1024:
            raise ValueError(f'Excessive lifecycle base: {relative}')
        data = source.read_bytes()
        digest = hashlib.sha1(f'blob {len(data)}\0'.encode() + data).hexdigest()
        if digest != entry.get('git_blob_sha'):
            raise ValueError(f'Pinned lifecycle-base hash mismatch: {relative}')
        target = tree / relative
        if target.exists() or target.is_symlink():
            raise ValueError(f'Lifecycle base would overwrite existing source: {relative}')
        ready.append((target, data))
    # Validate the complete inventory before publishing any file.
    for target, data in ready:
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)

def materialize(output: Path, repo: Path = REPO, kernel_source: Path | None = None) -> None:
    repo = repo.resolve()
    output = output.resolve()
    if kernel_source is None and os.environ.get('USB4_KERNEL_SOURCE'):
        kernel_source = Path(os.environ['USB4_KERNEL_SOURCE'])
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
        copy_external_base(repo, tree, kernel_source)
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
    parser.add_argument('--kernel-source', type=Path, help='Pinned upstream kernel files absent from the capture; never modified')
    args = parser.parse_args()
    try:
        materialize(args.output, kernel_source=args.kernel_source)
    except (OSError, ValueError, subprocess.CalledProcessError) as exc:
        parser.exit(1, f'{exc}\n')
