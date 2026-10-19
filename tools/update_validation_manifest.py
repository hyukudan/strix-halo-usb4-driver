#!/usr/bin/env python3
"""Regenerate provenance/software-validation.json from a reconstructed source.

--source is a tree with the complete series applied: either the output of
tools/prepare_overlay.py or a pinned kernel checkout after applying
patches/series (they are byte-identical for every file the series touches).
Every file the series creates or modifies is hashed, so the manifest can never
silently cover only a subset. The engine files are hashed from --engine-repo
and must carry a UAPI header byte-identical to the driver's; otherwise the
manifest is not written (Engine/driver UAPI drift).

Default: print the manifest. --write replaces the file in --driver-repo.
"""
import argparse
import json
from pathlib import Path
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import validate_native as vn  # noqa: E402


def build(driver, source, engine):
    patches = vn.series(driver)
    files = {}
    for name in sorted(vn.touched_files(patches)):
        path = source / name
        if path.is_symlink() or not path.is_file():
            raise ValueError(f'{name} is touched by the series but missing from {source}')
        files[name] = vn.sha256(path)
    engine_files = {}
    for rel in vn.ENGINE_FILES:
        path = engine / rel
        if path.is_symlink() or not path.is_file():
            raise ValueError(f'engine file missing: {rel}')
        engine_files[rel] = vn.sha256(path)
    if engine_files[vn.ENGINE_UAPI] != files[vn.DRIVER_UAPI]:
        raise ValueError(f'Engine/driver UAPI drift: {engine / vn.ENGINE_UAPI} differs from '
                         f'{source / vn.DRIVER_UAPI}; sync the engine header first')
    try:
        engine_commit = subprocess.check_output(['git', '-C', str(engine), 'rev-parse', 'HEAD'],
                                                text=True, env=vn.env_clean(),
                                                stderr=subprocess.DEVNULL).strip()
    except (OSError, subprocess.CalledProcessError):
        engine_commit = None
    return dict(schema='usb4-software-validation-v2', kernel=vn.PIN, patch_count=len(patches),
                last_patch=patches[-1].name, series_sha256=vn.series_digest(patches),
                materialized_files=files, engine_commit=engine_commit,
                engine_files_sha256=engine_files)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--driver-repo', type=Path, default=vn.ROOT)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--engine-repo', type=Path, required=True)
    parser.add_argument('--write', action='store_true')
    args = parser.parse_args(argv)
    driver = args.driver_repo.resolve()
    try:
        manifest = build(driver, args.source.resolve(), args.engine_repo.resolve())
    except (OSError, ValueError) as exc:
        print(f'update_validation_manifest: {exc}', file=sys.stderr)
        return 1
    text = json.dumps(manifest, indent=2) + '\n'
    if args.write:
        (driver / vn.MANIFEST).write_text(text)
        print(f'wrote {driver / vn.MANIFEST} ({len(manifest["materialized_files"])} files)')
    else:
        sys.stdout.write(text)
    return 0


if __name__ == '__main__':
    sys.exit(main())
