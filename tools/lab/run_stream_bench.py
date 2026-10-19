#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Run tools/stream_mem_bench on N stream pairs and keep everything needed to
reproduce or audit the result (docs/BENCHMARKS.md).

Senders run on this host, receivers on --peer (over ssh), each process pinned
with taskset. Before anything runs, both hosts' identities are recorded
(kernel release and cmdline, module srcversions and parameters, configfs
stream attributes, usb4stream-describe topology, ring budget, sha256 of the
benchmark binary) and the ring budget must pass. All raw output is kept:

  OUT/plan.json                     exact commands, sizes, affinities
  OUT/<role>-identity.json          per host, before the run
  OUT/<role>-<stream>.log           raw stdout/stderr of every process
  OUT/result.json                   per-stream JSON + aggregate + exit codes

The aggregate is total received bytes (minus each receiver's excluded first
read) divided by the common receiver-clock window: latest end minus earliest
start over all receivers (one host, one CLOCK_MONOTONIC). Nothing is
discarded; a failed or unverified stream makes the run invalid (exit 1).

usage example (the benchmark binary and this driver checkout must be at the
same paths on both hosts):
  tools/lab/run_stream_bench.py --peer cluster2 --out ~/bench/2026-10-20-4x \\
      --bench ~/bin/stream_mem_bench --bytes 8G --block 1M \\
      --pair /dev/tbstream0:/dev/tbstream0:2:2 --pair /dev/tbstream1:/dev/tbstream1:4:4 ...
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shlex
import subprocess
import sys
import time

REPO = Path(__file__).resolve().parents[2]
SIZE = re.compile(r'^(\d+)([KMG]?)$')
UNITS = {'': 1, 'K': 1 << 10, 'M': 1 << 20, 'G': 1 << 30}

IDENTITY = r'''
set -u
echo "release=$(uname -r)"
echo "cmdline=$(cat /proc/cmdline)"
for m in thunderbolt thunderbolt_stream thunderbolt_net; do
  echo "module.$m.srcversion=$(cat /sys/module/$m/srcversion 2>/dev/null || echo not-loaded)"
done
for p in /sys/module/thunderbolt/parameters/* /sys/module/thunderbolt_stream/parameters/*; do
  [ -r "$p" ] && echo "param.${p#/sys/module/}=$(cat "$p" 2>/dev/null)"
done
for a in /sys/kernel/config/thunderbolt/stream/*/*/*; do
  [ -f "$a" ] && echo "configfs.${a#/sys/kernel/config/thunderbolt/stream/}=$(cat "$a" 2>/dev/null)"
done
echo "bench.sha256=$(sha256sum "$1" | cut -d' ' -f1)"
echo "driver.git=$(git -C "$2" describe --always --dirty 2>/dev/null || echo unknown)"
'''


def parse_size(text):
    match = SIZE.match(text.upper())
    if not match:
        raise argparse.ArgumentTypeError(f'invalid size {text!r} (use N, NK, NM or NG)')
    return int(match[1]) * UNITS[match[2]]


def parse_pair(text):
    parts = text.split(':')
    if len(parts) != 4 or not all(parts) or not parts[2].isdecimal() or not parts[3].isdecimal():
        raise argparse.ArgumentTypeError('pair must be LOCAL_DEV:REMOTE_DEV:LOCAL_CPU:REMOTE_CPU')
    return dict(local=parts[0], remote=parts[1], local_cpu=int(parts[2]), remote_cpu=int(parts[3]))


def aggregate(receivers):
    """Aggregate over the common receiver-clock window; None if any stream is invalid."""
    if not receivers or any(r is None or r.get('mode') != 'recv' or r.get('verification') != 1
                            or r.get('bad_chunks') != 0 for r in receivers):
        return None
    start = min(r['start_monotonic'] for r in receivers)
    end = max(r['end_monotonic'] for r in receivers)
    payload = sum(r['bytes'] - r['first_bytes_excluded'] for r in receivers)
    return dict(streams=len(receivers), window_seconds=end - start, payload_bytes=payload,
                gbps=payload * 8e-9 / (end - start) if end > start else None,
                per_stream_gbps=[r['gbps'] for r in receivers],
                formula='sum(bytes - first_bytes_excluded) * 8 / (max(end) - min(start)), '
                        'receiver CLOCK_MONOTONIC')


def last_json(text):
    for line in reversed(text.splitlines()):
        line = line.strip()
        if line.startswith('{'):
            try:
                return json.loads(line)
            except ValueError:
                return None
    return None


def plan(args):
    send, recv = ('send', 'recv') if args.direction == 'send' else ('recv', 'send')
    streams = []
    for index, pair in enumerate(args.pair):
        local = ['taskset', '-c', str(pair['local_cpu']), str(args.bench), send, pair['local'],
                 str(args.bytes), str(args.block), '1']
        remote = ['taskset', '-c', str(pair['remote_cpu']), str(args.bench), recv, pair['remote'],
                  str(args.bytes), str(args.block), '1']
        streams.append(dict(index=index, pair=pair, local=local,
                            remote=['ssh', '-o', 'BatchMode=yes', args.peer, shlex.join(remote)]))
    return dict(schema='usb4-stream-bench-v1', peer=args.peer, direction=args.direction,
                bytes_per_stream=args.bytes, block_bytes=args.block, verify=1,
                receiver='peer' if args.direction == 'send' else 'local',
                streams=streams)


def identity(command, bench, repo):
    result = subprocess.run(command + ['bash', '-s', '--', str(bench), str(repo)],
                            input=IDENTITY, capture_output=True, text=True, timeout=60)
    values = dict(line.split('=', 1) for line in result.stdout.splitlines() if '=' in line)
    return dict(values=values, returncode=result.returncode, stderr=result.stderr)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--peer', required=True, help='ssh name of the other host')
    parser.add_argument('--out', type=Path, required=True, help='new directory for all records')
    parser.add_argument('--bench', type=Path, required=True,
                        help='stream_mem_bench binary, same path on both hosts')
    parser.add_argument('--bytes', type=parse_size, default=parse_size('8G'))
    parser.add_argument('--block', type=parse_size, default=parse_size('1M'))
    parser.add_argument('--pair', type=parse_pair, action='append', required=True)
    parser.add_argument('--direction', choices=('send', 'recv'), default='send',
                        help='what this host does (default: this host sends)')
    parser.add_argument('--dry-run', action='store_true', help='print the plan only')
    args = parser.parse_args(argv)
    if not 1 <= args.block <= 64 << 20:
        parser.error('--block must be 1..64M')
    the_plan = plan(args)
    if args.dry_run:
        print(json.dumps(the_plan, indent=2))
        return 0
    args.out.mkdir(parents=True, exist_ok=False)
    (args.out / 'plan.json').write_text(json.dumps(the_plan, indent=2) + '\n')
    ssh = ['ssh', '-o', 'BatchMode=yes', args.peer]
    for role, prefix in (('local', []), ('peer', ssh)):
        ident = identity(prefix, args.bench, REPO)
        diag = subprocess.run(prefix + ['python3', str(REPO / 'tools/usb4_diagnose.py'),
                                        '--require-budget'], capture_output=True, text=True,
                              timeout=60)
        topo = subprocess.run(prefix + ['python3', str(REPO / 'tools/lab/usb4stream_describe.py')],
                              capture_output=True, text=True, timeout=60)
        ident.update(diagnose=diag.stdout, diagnose_rc=diag.returncode, diagnose_err=diag.stderr,
                     topology=topo.stdout, topology_rc=topo.returncode)
        (args.out / f'{role}-identity.json').write_text(json.dumps(ident, indent=2) + '\n')
        if diag.returncode:
            print(f'{role}: ring budget check failed; not running:\n{diag.stderr}', file=sys.stderr)
            return 1
    receivers_cmd = 'remote' if args.direction == 'send' else 'local'
    procs = []
    for stream in the_plan['streams']:
        for role in (receivers_cmd, 'local' if receivers_cmd == 'remote' else 'remote'):
            log = (args.out / f'{"peer" if role == "remote" else "local"}-{stream["index"]}.log').open('w')
            procs.append((stream['index'], role, log,
                          subprocess.Popen(stream[role], stdout=log, stderr=subprocess.STDOUT)))
            if role == receivers_cmd:
                time.sleep(0.5)          # receivers open first
    codes = {}
    for index, role, log, proc in procs:
        codes[f'{role}-{index}'] = proc.wait(timeout=3600)
        log.close()
    results, receivers = [], []
    for stream in the_plan['streams']:
        entry = dict(index=stream['index'], pair=stream['pair'])
        for role, name in (('local', 'local'), ('remote', 'peer')):
            entry[role] = last_json((args.out / f'{name}-{stream["index"]}.log').read_text())
        receivers.append(entry['remote' if args.direction == 'send' else 'local'])
        results.append(entry)
    summary = dict(exit_codes=codes, streams=results, aggregate=aggregate(receivers),
                   valid=all(c == 0 for c in codes.values()) and aggregate(receivers) is not None)
    (args.out / 'result.json').write_text(json.dumps(summary, indent=2) + '\n')
    print(json.dumps(summary['aggregate'], indent=2))
    return 0 if summary['valid'] else 1


if __name__ == '__main__':
    sys.exit(main())
