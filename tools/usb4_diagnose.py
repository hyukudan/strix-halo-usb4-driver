#!/usr/bin/env python3
"""Read-only USB4 link report; speeds in sysfs are per lane, not per cable."""
import argparse
import json
import os
import platform
from pathlib import Path
import re
import subprocess
import sys

SPEED = re.compile(r'^\s*(\d+(?:\.\d+)?)\s+Gb/s\s*$')
# Data ring pairs per AMD Strix Halo NHI that the lab gives to USB4STREAM
# (two streams per port). USB4NET takes one of them when it binds a service.
LAB_DATA_RING_PAIRS = 2
POLICY = 'usb4stream-dedicated.conf'
SERVICE = re.compile(r'^(\d+)-[0-9a-f]+\.\d+$')

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

def ring_budget(root, configfs=None, modprobe_dir=Path('/etc/modprobe.d'), kernel=None):
    """Ring-pair consumers per Thunderbolt domain against the lab budget.

    Consumers: configured USB4STREAM endpoints (configfs groups) and services
    bound to thunderbolt-net. Read-only; problems are listed, never fixed.
    """
    configfs = Path(configfs) if configfs else root / 'kernel/config/thunderbolt/stream'
    domains = {}

    def slot(domain):
        return domains.setdefault(domain, dict(streams=[], usb4net_services=[]))

    try:
        groups = sorted(configfs.iterdir()) if configfs.is_dir() else []
    except OSError:
        groups = []
    for group in groups:
        match = SERVICE.match(group.name)
        if not match:
            continue
        try:
            names = sorted(p.name for p in group.iterdir() if p.is_dir())
        except OSError:
            continue
        slot(int(match[1]))['streams'] += [f'{group.name}/{n}' for n in names]
    devices = root / 'bus/thunderbolt/devices'
    for dev in sorted(devices.glob('*')) if devices.is_dir() else []:
        match = SERVICE.match(dev.name)
        if not match:
            continue
        try:
            driver = Path(os.readlink(dev / 'driver')).name
        except OSError:
            continue
        if driver in ('thunderbolt-net', 'thunderbolt_net'):
            slot(int(match[1]))['usb4net_services'].append(dev.name)
    problems = []
    for domain, item in sorted(domains.items()):
        item['budget'] = LAB_DATA_RING_PAIRS
        item['used'] = len(item['streams']) + len(item['usb4net_services'])
        if item['usb4net_services']:
            problems.append(f'domain {domain}: thunderbolt-net bound to '
                            f'{", ".join(item["usb4net_services"])} takes a ring pair')
        if item['used'] > item['budget']:
            problems.append(f'domain {domain}: {item["used"]} ring-pair consumers for a budget '
                            f'of {item["budget"]}; some stream opens will fail')
    net_loaded = (root / 'module/thunderbolt_net').exists()
    policy = Path(modprobe_dir) / POLICY
    lab_kernel = (kernel or platform.release()).startswith('7.3.0-rc5-strix-gpu-bosgame-')
    if lab_kernel and not policy.is_file():
        problems.append(f'{policy} missing: thunderbolt_net can load and take a ring pair '
                        '(install tools/lab/usb4stream-dedicated.conf)')
    if lab_kernel and net_loaded:
        problems.append('thunderbolt_net is loaded on a lab kernel')
    return dict(domains={str(d): v for d, v in sorted(domains.items())},
                thunderbolt_net_loaded=net_loaded, modprobe_policy=str(policy),
                modprobe_policy_present=policy.is_file(), problems=problems)


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

# -- Lane adapter registers (debugfs, root) -----------------------------------
# Field layout from drivers/thunderbolt/tb_regs.h (lane adapter capability,
# TB_PORT_CAP_PHY = 0x01): LANE_ADP_CS_0 at +0, LANE_ADP_CS_1 at +1.
CAP_LANE = 0x01
CUR_SPEED = {0x8: ('Gen2', 10.0), 0x4: ('Gen3', 20.0), 0x2: ('Gen4', 40.0)}
TARGET_SPEED_GEN3 = 0xc
ROUTER = re.compile(r'^(\d+)-0$')
XDOMAIN = re.compile(r'^(\d+)-([0-9a-f]+)$')


def bits(value, high, low):
    return (value >> low) & ((1 << (high - low + 1)) - 1)


def decode_lane(cs0, cs1):
    """LANE_ADP_CS_0/1 decoded exactly as tb_regs.h defines the fields."""
    cur = bits(cs1, 19, 16)
    return dict(
        cs0=f'0x{cs0:08x}', cs1=f'0x{cs1:08x}',
        supported_speed=f'0x{bits(cs0, 19, 16):x}', supported_width=bits(cs0, 25, 20),
        cl0s_support=bool(cs0 >> 26 & 1), cl1_support=bool(cs0 >> 27 & 1),
        cl2_support=bool(cs0 >> 28 & 1),
        target_speed=f'0x{bits(cs1, 3, 0):x}', target_width=bits(cs1, 5, 4),
        cl0s_enable=bool(cs1 >> 10 & 1), cl1_enable=bool(cs1 >> 11 & 1),
        cl2_enable=bool(cs1 >> 12 & 1),
        lane_disabled=bool(cs1 >> 14 & 1), lane_bonded=bool(cs1 >> 15 & 1),
        cur_speed=f'0x{cur:x}', cur_generation=CUR_SPEED.get(cur, ('unknown', None))[0],
        cur_gbps_per_lane=CUR_SPEED.get(cur, (None, None))[1], cur_width=bits(cs1, 25, 20),
        pms=bool(cs1 >> 30 & 1))


def parse_regs(text):
    """debugfs `regs`: '# offset relative_offset cap_id vs_cap_id value' rows."""
    rows = []
    for line in (text or '').splitlines():
        parts = line.split()
        if len(parts) == 5 and not line.startswith('#'):
            try:
                rows.append(tuple(int(p, 0) for p in parts))
            except ValueError:
                continue
    return rows


def lane_cs(rows):
    found = {rel: value for _off, rel, cap, _vs, value in rows if cap == CAP_LANE and rel in (0, 1)}
    return (found[0], found[1]) if 0 in found and 1 in found else None


def parse_sb(text):
    """debugfs `sb_regs`: '0x0c 0x03 0xf3 0x03' -> {0x0c: [3, 0xf3, 3]}; None if not accessible."""
    regs = {}
    for line in (text or '').splitlines():
        parts = line.split()
        if not parts or line.startswith('#'):
            continue
        try:
            reg = int(parts[0], 0)
            regs[reg] = None if '<not' in line else [int(p, 0) for p in parts[1:]]
        except ValueError:
            continue
    return regs


def read_debugfs(path):
    try:
        return path.read_text(), None
    except OSError as exc:
        return None, f'{path}: {exc.strerror or exc}'


def lane_registers(root, debugfs):
    """Both lane adapters of every USB4 port of every host router, compared with sysfs."""
    devices = root / 'bus/thunderbolt/devices'
    result, mismatches, warnings, errors = [], [], [], []
    names = sorted(p.name for p in devices.glob('*')) if devices.is_dir() else []
    for router in names:
        match = ROUTER.match(router)
        if not match:
            continue
        domain = int(match[1])
        ports = sorted(int(p.name[9:]) for p in (devices / router).glob('usb4_port*')
                       if p.name[9:].isdecimal())
        for port in ports:
            entry = dict(domain=domain, router=router, usb4_port=port, adapters={}, sideband={})
            for adapter in (port, port + 1):      # lane 0 and lane 1 adapters
                text, err = read_debugfs(debugfs / router / f'port{adapter}' / 'regs')
                if err:
                    errors.append(err)
                    continue
                cs = lane_cs(parse_regs(text))
                entry['adapters'][str(adapter)] = decode_lane(*cs) if cs else None
            text, err = read_debugfs(debugfs / router / f'port{port}' / 'sb_regs')
            if err:
                errors.append(err)
            else:
                sb = parse_sb(text)
                link_conf, txffe = sb.get(0x0c), sb.get(0x0d)
                entry['sideband'] = dict(
                    link_conf=[f'0x{b:02x}' for b in link_conf] if link_conf else None,
                    txffe=[f'0x{b:02x}' for b in txffe] if txffe else None,
                    # Lab decode (byte 1 bit 5), not yet checked against the USB4 spec table.
                    gen3_support_lab_decode=bool(link_conf[1] >> 5 & 1)
                    if link_conf and len(link_conf) > 1 else None)
            peer = next((n for n in names if XDOMAIN.match(n) and int(XDOMAIN.match(n)[1]) == domain
                         and int(XDOMAIN.match(n)[2], 16) & 0xff == port
                         and (devices / n / 'rx_speed').exists()), None)
            if peer:
                entry['sysfs'] = dict(xdomain=peer, rx=direction(devices / peer, 'rx'),
                                      tx=direction(devices / peer, 'tx'))
            entry['mismatches'], entry['warnings'] = compare_lanes(entry)
            mismatches += [f'{router} port{port}: {p}' for p in entry['mismatches']]
            warnings += [f'{router} port{port}: {p}' for p in entry['warnings']]
            result.append(entry)
    return dict(ports=result, mismatches=mismatches, warnings=warnings, errors=sorted(set(errors)),
                note='LANE_ADP_CS_0/1 decoded per tb_regs.h; current speed 0x4 = Gen3 '
                     '(20 Gb/s per lane), 0x8 = Gen2 (10 Gb/s). sysfs holds the speed cached at '
                     'enumeration; the registers hold the live state.')


def compare_lanes(entry):
    """(mismatches, warnings): sysfs vs registers disagreements; degraded but valid states."""
    problems, warnings = [], []
    lane0 = entry['adapters'].get(str(entry['usb4_port']))
    lanes = [lane for lane in entry['adapters'].values() if lane]
    sysfs = entry.get('sysfs')
    if lane0 and sysfs:
        for which in ('rx', 'tx'):
            d = sysfs[which]
            if d['speed_gbps_per_lane'] is not None and \
                    d['speed_gbps_per_lane'] != lane0['cur_gbps_per_lane']:
                problems.append(f'sysfs {which}_speed {d["speed_gbps_per_lane"]:g} Gb/s but '
                                f'LANE_ADP_CS_1 current speed {lane0["cur_speed"]} '
                                f'({lane0["cur_generation"]})')
            if d['lanes'] is not None and d['lanes'] != lane0['cur_width']:
                problems.append(f'sysfs {which}_lanes {d["lanes"]} but LANE_ADP_CS_1 current '
                                f'width {lane0["cur_width"]}')
    for lane in lanes:
        if lane['target_speed'] == f'0x{TARGET_SPEED_GEN3:x}' and lane['cur_generation'] == 'Gen2':
            warnings.append(f'trained Gen2 although the target speed is Gen3 '
                            f'(CS1 {lane["cs1"]})')
            break
    if len({(l['cur_speed'], l['cur_width']) for l in lanes}) > 1:
        warnings.append('the two lane adapters disagree on current speed/width')
    if any(l['lane_disabled'] for l in lanes):
        warnings.append('a lane adapter is disabled (LANE_ADP_CS_1.LD)')
    gen3 = entry['sideband'].get('gen3_support_lab_decode')
    if gen3 is False:
        warnings.append('sideband LINK_CONF advertises no Gen3 support (lab decode)')
    return problems, warnings


def lanecap_text(report, host):
    """The capture format used in the lab notes (lanecap-*.txt)."""
    out = [f'### {host} {report["kernel"]}']
    for entry in report['lanes']['ports']:
        sysfs = entry.get('sysfs')
        head = f'domain{entry["domain"]} port{entry["usb4_port"]}'
        if sysfs:
            head += (f' sysfs rx={sysfs["rx"]["raw_speed"]} x{sysfs["rx"]["lanes"]} '
                     f'tx={sysfs["tx"]["raw_speed"]} x{sysfs["tx"]["lanes"]} peer={sysfs["xdomain"]}')
        out.append(head)
        for adapter, lane in entry['adapters'].items():
            if lane:
                out.append(f'  {entry["router"]} port{adapter} CS0={lane["cs0"]} CS1={lane["cs1"]} '
                           f'target_speed={lane["target_speed"]} target_width={lane["target_width"]} '
                           f'cur_speed={lane["cur_speed"]} cur_width={lane["cur_width"]} '
                           f'lane_dis={int(lane["lane_disabled"])} lane_bond={int(lane["lane_bonded"])}')
        sb = entry['sideband']
        if sb:
            out.append('  LINK_CONF 0x0c ' + ' '.join(sb['link_conf'] or ['<n/a>']) +
                       '  TXFFE 0x0d ' + ' '.join(sb['txffe'] or ['<n/a>']))
        out += [f'  ! mismatch: {p}' for p in entry['mismatches']]
        out += [f'  ! {p}' for p in entry['warnings']]
    out += [f'! {e}' for e in report['lanes']['errors']]
    return '\n'.join(out)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sysfs-root', type=Path, default=Path('/sys'))
    parser.add_argument('--logs', action='store_true', help='Also read recent filtered kernel logs')
    parser.add_argument('--modprobe-dir', type=Path, default=Path('/etc/modprobe.d'))
    parser.add_argument('--require-budget', action='store_true',
                        help='exit 1 if USB4NET or extra consumers take the lab ring pairs '
                             '(run before every benchmark)')
    parser.add_argument('--registers', action='store_true',
                        help='also read LANE_ADP_CS_0/1 of both lane adapters of every USB4 port '
                             'and sideband LINK_CONF/TXFFE from debugfs (root; read-only) and '
                             'compare them with sysfs; exit 1 on a mismatch')
    parser.add_argument('--debugfs-root', type=Path, default=Path('/sys/kernel/debug/thunderbolt'))
    parser.add_argument('--lanecap', action='store_true',
                        help='with --registers, print the lab lanecap text instead of JSON')
    args = parser.parse_args(argv)
    report = collect(args.sysfs_root)
    report['ring_budget'] = ring_budget(args.sysfs_root, modprobe_dir=args.modprobe_dir)
    if args.registers or args.lanecap:
        report['lanes'] = lane_registers(args.sysfs_root, args.debugfs_root)
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
    if args.lanecap:
        print(lanecap_text(report, platform.node()))
    else:
        print(json.dumps(report, indent=2))
    status = 0
    lanes = report.get('lanes')
    if lanes:
        for line in lanes['mismatches']:
            print(f'lane registers: sysfs/register mismatch: {line}', file=sys.stderr)
        for line in lanes['errors']:
            print(f'lane registers: {line}', file=sys.stderr)
        if lanes['mismatches'] or (lanes['errors'] and not lanes['ports']):
            status = 1
        if lanes['errors'] and lanes['ports'] and all(
                not any(e['adapters'].values()) for e in lanes['ports']):
            status = 1      # nothing readable (not root?): no comparison happened
    problems = report['ring_budget']['problems']
    if args.require_budget and problems:
        for problem in problems:
            print(f'ring budget: {problem}', file=sys.stderr)
        return 1
    return status

if __name__ == '__main__':
    sys.exit(main())
