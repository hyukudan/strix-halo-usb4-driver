#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Fail on compiler/modpost diagnostics that concern this series' files.

A full `make W=1 vmlinux modules` of an upstream tree prints W=1 warnings in
unrelated subsystems; those are not ours to fix. Every warning or error whose
location is a file the series owns (drivers/thunderbolt/, drivers/net/thunderbolt/,
include/linux/thunderbolt.h, include/uapi/linux/strix_gpu_stream.h), and every
modpost diagnostic about the USB4 modules, fails the check. The build log must
also show the three USB4 modules being linked (modpost ran on them).

usage: check_build_log.py BUILD.LOG
"""
import argparse
from pathlib import Path
import re
import sys

OURS = re.compile(r'(?:^|[\s/])(drivers/thunderbolt/|drivers/net/thunderbolt/|'
                  r'include/linux/thunderbolt\.h|include/uapi/linux/strix_gpu_stream\.h)')
DIAG = re.compile(r'\b(warning|error):', re.I)
MODPOST = re.compile(r'(WARNING|ERROR): modpost: .*(thunderbolt|strix)', re.I)
MODULES = ('drivers/thunderbolt/thunderbolt.ko', 'drivers/thunderbolt/thunderbolt_stream.ko',
           'drivers/net/thunderbolt/thunderbolt_net.ko')


def check(text, modules=MODULES):
    problems = []
    for number, line in enumerate(text.splitlines(), 1):
        if MODPOST.search(line) or (DIAG.search(line) and OURS.search(line)):
            problems.append(f'line {number}: {line.strip()}')
    for module in modules:
        # Kbuild prints "  LD [M]  path.ko" when it links a module after modpost.
        if not re.search(r'^\s*LD \[M\]\s+(?:\S*/)?' + re.escape(module) + r'\s*$', text, re.M):
            problems.append(f'{module} was not linked (no "LD [M]" line): modpost/link not proven')
    return problems


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('log', type=Path)
    args = parser.parse_args(argv)
    problems = check(args.log.read_text(errors='replace'))
    for problem in problems:
        print(f'check_build_log: {problem}', file=sys.stderr)
    if not problems:
        print(f'check_build_log: {args.log}: no diagnostics in USB4 files; all modules linked')
    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main())
