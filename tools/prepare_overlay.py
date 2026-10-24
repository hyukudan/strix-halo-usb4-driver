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


def tree_inventory(root: Path) -> set[str]:
    """Relative POSIX paths of every regular file below root.

    Symlinks (to files or directories) and special files are rejected instead of
    being followed or copied, so nothing outside the hashed inventory slips in.
    """
    found = set()
    for top, dirs, files in os.walk(root, followlinks=False):
        for name in dirs + files:
            path = Path(top) / name
            if path.is_symlink():
                raise ValueError(f'Symlink not allowed: {path.relative_to(root).as_posix()}')
            if name in files and not path.is_file():
                raise ValueError(f'Special file not allowed: {path.relative_to(root).as_posix()}')
        found.update((Path(top) / name).relative_to(root).as_posix() for name in files)
    return found


def safe_relative(name: str) -> Path:
    relative = Path(name)
    if (not isinstance(name, str) or relative.is_absolute() or '..' in relative.parts
            or not relative.parts or relative.as_posix() != name):
        raise ValueError(f'Invalid inventory path: {name!r}')
    return relative


def copy_overlay(repo: Path, tree: Path, manifest: dict) -> None:
    """Copy exactly the manifest's captured files, each verified as it is read.

    Any extra, missing, duplicate or symlinked entry under overlay/ fails before
    anything is published; only the bytes that were hashed are written.
    """
    overlay = repo / 'overlay'
    if overlay.is_symlink() or not overlay.is_dir():
        raise ValueError('overlay/ must be a real directory')
    items = manifest['modified_files']
    names = [item['path'] for item in items]
    if len(set(names)) != len(names):
        raise ValueError('Duplicate overlay inventory path')
    actual = tree_inventory(overlay)
    extra, missing = sorted(actual - set(names)), sorted(set(names) - actual)
    if extra or missing:
        raise ValueError(f'Overlay inventory mismatch: unexpected {extra}, missing {missing}')
    ready = []
    for item in items:
        relative = safe_relative(item['path'])
        data = (overlay / relative).read_bytes()
        if hashlib.sha256(data).hexdigest() != item['snapshot_sha256']:
            raise ValueError(f"Snapshot hash mismatch: {overlay / relative}")
        ready.append((tree / relative, data))
    tree.mkdir()
    for target, data in ready:
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)


def audit_series(repo: Path) -> list[Path]:
    """patches/audit-series must be patches/series without 0001, all regular files."""
    def entries(name):
        return [line for line in (repo / 'patches' / name).read_text().splitlines()
                if line and not line.startswith('#')]
    audit = entries('audit-series')
    series_path = repo / 'patches/series'
    if series_path.exists() and audit != entries('series')[1:]:
        raise ValueError('patches/audit-series must equal patches/series without 0001')
    result = []
    for name in audit:
        if Path(name).name != name or not name.endswith('.patch'):
            raise ValueError(f'Invalid patch name: {name}')
        patch = repo / 'patches' / name
        if patch.is_symlink() or not patch.is_file():
            raise ValueError(f'Missing or symlinked patch: {name}')
        result.append(patch)
    return result


def experimental_series(repo: Path) -> list[Path]:
    """patches/experimental/series: regular files in that directory, in order."""
    directory = repo / 'patches/experimental'
    result = []
    for name in (directory / 'series').read_text().splitlines():
        if not name or name.startswith('#'):
            continue
        if Path(name).name != name or not name.endswith('.patch'):
            raise ValueError(f'Invalid patch name: {name}')
        patch = directory / name
        if patch.is_symlink() or not patch.is_file():
            raise ValueError(f'Missing or symlinked patch: {name}')
        result.append(patch)
    return result


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
    actual = tree_inventory(base) if base.is_dir() and not base.is_symlink() else None
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

def materialize(output: Path, repo: Path = REPO, kernel_source: Path | None = None,
                experimental: bool = False) -> None:
    repo = repo.resolve()
    output = output.resolve()
    if kernel_source is None and os.environ.get('USB4_KERNEL_SOURCE'):
        kernel_source = Path(os.environ['USB4_KERNEL_SOURCE'])
    if output.exists():
        raise ValueError(f"Refusing to overwrite {output}")
    manifest = json.loads((repo / 'provenance/source-manifest.json').read_text())
    patches = audit_series(repo)
    if experimental:
        patches += experimental_series(repo)
    original = repo / 'patches/0001-usb4-current-source.patch'
    if hashlib.sha256(original.read_bytes()).hexdigest() != manifest['combined_patch_sha256']:
        raise ValueError('Original combined patch hash mismatch')
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='.usb4-', dir=output.parent) as tmp:
        tree = Path(tmp) / 'source'
        copy_overlay(repo, tree, manifest)
        copy_additional_base(repo, tree)
        copy_external_base(repo, tree, kernel_source)
        # Never inherit a surrounding repository: git apply otherwise silently
        # skips these paths when output is a subdirectory of the checkout.
        env = os.environ.copy()
        for key in ('GIT_DIR', 'GIT_WORK_TREE', 'GIT_INDEX_FILE'):
            env.pop(key, None)
        env['GIT_CEILING_DIRECTORIES'] = str(tree.parent)
        for patch in patches:
            subprocess.run(['git', 'apply', '--check', str(patch)], cwd=tree, env=env, check=True)
            subprocess.run(['git', 'apply', '--whitespace=error-all', str(patch)], cwd=tree, env=env, check=True)
        shutil.move(str(tree), str(output))
    print(f'Audited overlay: {output}')

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path, help='New, non-existing directory')
    parser.add_argument('--kernel-source', type=Path, help='Pinned upstream kernel files absent from the capture; never modified')
    parser.add_argument('--experimental', action='store_true',
                        help='Also apply patches/experimental/series on top of the stable series')
    args = parser.parse_args()
    try:
        materialize(args.output, kernel_source=args.kernel_source, experimental=args.experimental)
    except (OSError, ValueError, subprocess.CalledProcessError) as exc:
        parser.exit(1, f'{exc}\n')
