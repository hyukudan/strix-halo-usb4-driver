#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Check a final .config (after olddefconfig) against config/usb4-gpu.fragment.

Kconfig silently drops options whose dependencies are unmet (for example
USB4_CONFIGFS and USB4_STREAM without CONFIGFS_FS), so merging the fragment is
not proof that the driver is enabled. This compares every fragment setting with
the final value and enforces a per-build kernel identity:

* CONFIG_LOCALVERSION must start with the lab prefix (-strix-gpu-bosgame-) so
  the lab modprobe policy and service condition apply, must not be the fragment
  placeholder and must not be a previously used identity (--forbid, the
  captured -strix-gpu-bosgame-20261014 is always forbidden);
* with --kernelrelease, <modules-root>/<kernelrelease> must not exist yet, so a
  new build can never overwrite the modules of an installed (fallback) kernel.

Read-only. Exit status 0 when every check passes, 1 otherwise.
"""
import argparse
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
FRAGMENT = ROOT / 'config/usb4-gpu.fragment'
LAB_PREFIX = '-strix-gpu-bosgame-'
PLACEHOLDER = LAB_PREFIX + 'SET-A-UNIQUE-BUILD-ID'
CAPTURED = (LAB_PREFIX + '20261014',)
SUFFIX = re.compile(r'^[A-Za-z0-9][A-Za-z0-9._+-]{0,40}$')
LINE = re.compile(r'^(CONFIG_[A-Za-z0-9_]+)=(.*)$')
UNSET = re.compile(r'^# (CONFIG_[A-Za-z0-9_]+) is not set$')


def parse(text):
    """{CONFIG_NAME: value}; 'n' for '# ... is not set'."""
    values = {}
    for line in text.splitlines():
        line = line.strip()
        match = LINE.match(line) or UNSET.match(line)
        if match:
            values[match[1]] = match[2] if match.re is LINE else 'n'
    return values


def check(config_text, fragment_text, gpu=True, forbid=(), kernelrelease=None,
          modules_root=Path('/lib/modules')):
    """List of problems; empty when the final configuration is acceptable."""
    final, wanted = parse(config_text), parse(fragment_text)
    if gpu is False:
        wanted['CONFIG_USB4_GPU_STREAM'] = 'n'
    problems = []
    for name, value in wanted.items():
        if name == 'CONFIG_LOCALVERSION':
            continue
        actual = final.get(name, 'n')
        if actual != value:
            problems.append(f'{name}: fragment wants {value}, final .config has {actual}'
                            ' (unmet dependency or overridden)')
    local = final.get('CONFIG_LOCALVERSION', '""')
    local = local[1:-1] if len(local) >= 2 and local[0] == local[-1] == '"' else local
    if local == PLACEHOLDER:
        problems.append('CONFIG_LOCALVERSION is still the fragment placeholder: set a unique '
                        f'identity, e.g. scripts/config --set-str LOCALVERSION '
                        f'"{LAB_PREFIX}$(date +%Y%m%d)a"')
    elif not local.startswith(LAB_PREFIX) or not SUFFIX.match(local[len(LAB_PREFIX):]):
        problems.append(f'CONFIG_LOCALVERSION {local!r} must be {LAB_PREFIX}<build-id> '
                        '(letters, digits, . _ + -) so the lab policy matches it')
    elif local in set(CAPTURED) | set(forbid):
        problems.append(f'CONFIG_LOCALVERSION {local!r} was already used by another build; '
                        'pick a new one so modules and the fallback kernel are not overwritten')
    if kernelrelease is not None:
        if not kernelrelease.endswith(local):
            problems.append(f'kernelrelease {kernelrelease!r} does not end with LOCALVERSION '
                            f'{local!r} (LOCALVERSION_AUTO or a stale build directory?)')
        target = Path(modules_root) / kernelrelease
        if target.exists():
            problems.append(f'{target} already exists: installing this build would replace '
                            'the modules of an installed kernel')
    return problems


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('config', type=Path, help='final .config (after olddefconfig)')
    parser.add_argument('--fragment', type=Path, default=FRAGMENT)
    parser.add_argument('--gpu-off', action='store_true',
                        help='the build deliberately disables USB4_GPU_STREAM')
    parser.add_argument('--forbid', action='append', default=[],
                        help='LOCALVERSION already used (repeatable), e.g. the running kernel')
    parser.add_argument('--kernelrelease', help='output of make -s kernelrelease')
    parser.add_argument('--modules-root', type=Path, default=Path('/lib/modules'))
    args = parser.parse_args(argv)
    problems = check(args.config.read_text(), args.fragment.read_text(), not args.gpu_off,
                     args.forbid, args.kernelrelease, args.modules_root)
    for problem in problems:
        print(f'check_kconfig: {problem}', file=sys.stderr)
    if not problems:
        print(f'check_kconfig: {args.config}: all fragment settings present; identity OK')
    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main())
