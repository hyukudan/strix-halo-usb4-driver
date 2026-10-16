#!/usr/bin/env python3
"""Read-only USB4 link report; speeds in sysfs are per lane, not per cable."""
import argparse
import json
import platform
from pathlib import Path
import re
import subprocess

SPEED = re.compile(r'^\s*(\d+(?:\.\d+)?)\s+Gb/s\s*$')

def read(path):
    try:
        return path.read_text().strip()
    except OSError:
        return None

def direction(device, which):
    raw_speed = read(device / f'{which}_speed')
    raw_lanes = read(device / f'{which}_lanes')
    match = SPEED.fullmatch(raw_speed or '')
    lanes = int(raw_lanes) if raw_lanes and raw_lanes.isdecimal() else None
    speed = float(match[1]) if match else None
    # Report invalid/absent data as unknown, never infer that bonding failed.
    total = speed * lanes if speed and lanes in (1, 2, 3) else None
    return dict(speed_gbps_per_lane=speed, lanes=lanes, raw_gbps=total,
                raw_speed=raw_speed, raw_lanes=raw_lanes)

def collect(root):
    links = []
    for device in sorted((root / 'bus/thunderbolt/devices').glob('*')):
        if not (device / 'rx_speed').exists() and not (device / 'tx_speed').exists():
            continue
        item = dict(device=device.name, path=str(device.resolve()),
                    rx=direction(device, 'rx'), tx=direction(device, 'tx'))
        # Map the router/xdomain back to its actual PCI controller.
        for parent in (device.resolve(), *device.resolve().parents):
            if (parent / 'class').exists() and (parent / 'vendor').exists():
                item['controller'] = dict(bdf=parent.name, vendor=read(parent/'vendor'),
                                          device=read(parent/'device'))
                break
        links.append(item)
    params = {}
    for module in ('thunderbolt', 'thunderbolt_stream', 'thunderbolt_net'):
        base = root / 'module' / module
        params[module] = dict(loaded=base.exists(), srcversion=read(base/'srcversion'))
    params['dma_credits_for_new_tunnels'] = read(root/'module/thunderbolt/parameters/dma_credits')
    return dict(kernel=platform.release(), links=links, modules=params,
                note='raw_gbps = Gb/s per lane multiplied by lane count, per direction; not application throughput. No hardware or configuration changes made.')

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sysfs-root', type=Path, default=Path('/sys'))
    parser.add_argument('--logs', action='store_true', help='Also read recent filtered kernel logs')
    args = parser.parse_args()
    report = collect(args.sysfs_root)
    if args.logs:
        try:
            result = subprocess.run(['journalctl', '-k', '-b', '--no-pager', '-n', '2000'],
                                    text=True, capture_output=True, timeout=15)
            report['kernel_log'] = [line for line in result.stdout.splitlines()
                                    if re.search(r'thunderbolt|usb4|lane bonding|\bnhi\b', line, re.I)]
            if result.returncode or result.stderr.strip():
                report['log_warning'] = result.stderr.strip() or 'journalctl failed'
        except (OSError, subprocess.TimeoutExpired) as exc:
            report['log_warning'] = str(exc)
    print(json.dumps(report, indent=2))

if __name__ == '__main__':
    main()
