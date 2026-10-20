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

MSIX = re.compile(r'PCI-MSIX-(\S+)\s+(\d+)-edge\s+(.*)$')
CONTROL_RINGS = 2        # control TX + RX hold the first two vectors of every NHI
EVIDENCE = (('GPU context still quarantined after rmdir', 'orphaned GPU context kept after rmdir'),
            ('GPU contexts retained', 'GPU contexts retained (DMA stop unproved)'),
            ('leaked by an abandoned DMA owner', 'ring leaked by an abandoned DMA owner'),
            ('host interface reset after DMA teardown pending', 'teardown reset pending'))


def nhi_by_domain(root):
    """{domain: PCI address} from sysfs (bus/pci/devices/<bdf>/domainN)."""
    out = {}
    base = root / 'bus/pci/devices'
    for pci in sorted(base.glob('*')) if base.is_dir() else []:
        for child in pci.glob('domain*'):
            if child.name[6:].isdecimal():
                out[int(child.name[6:])] = pci.name
    return out


def held_rings(interrupts_text):
    """{PCI address: data rings holding an MSI-X vector} from /proc/interrupts."""
    vectors = {}
    for line in interrupts_text.splitlines():
        match = MSIX.search(line)
        if match and 'thunderbolt' in match[3].split(', '):
            vectors[match[1]] = vectors.get(match[1], 0) + 1
    return {pci: max(0, count - CONTROL_RINGS) for pci, count in vectors.items()}


def stream_openers(proc=Path('/proc')):
    """({tbstream index: [pid, ...]}, number of processes whose fds could not be read).

    Read-only scan of /proc/<pid>/fd links to /dev/tbstreamN. Without root,
    other users' processes are unreadable; the caller must treat that as unknown.
    """
    openers, unreadable = {}, 0
    for pid in sorted(proc.glob('[0-9]*')):
        try:
            fds = list((pid / 'fd').iterdir())
        except FileNotFoundError:
            continue                    # exited meanwhile
        except OSError:
            unreadable += 1
            continue
        for fd in fds:
            try:
                target = os.readlink(fd)
            except OSError:
                continue
            match = re.fullmatch(r'/dev/tbstream(\d+)', target)
            if match:
                openers.setdefault(int(match[1]), []).append(int(pid.name))
    return openers, unreadable


def ring_budget(root, configfs=None, modprobe_dir=Path('/etc/modprobe.d'), kernel=None,
                interrupts=Path('/proc/interrupts'), kernel_log=None, allow_held=False,
                log_error=None, openers=None):
    """Ring-pair consumers per Thunderbolt domain against the lab budget.

    Two views, both read-only:
    - planned: configured USB4STREAM endpoints (configfs groups) and services
      bound to thunderbolt-net;
    - held now: data rings that hold an MSI-X vector of the domain's NHI in
      /proc/interrupts. This includes open streams, USB4NET and GPU contexts
      retained after close or rmdir (orphans), which no configfs group shows.
    Rings created without interrupts (busy_poll streams) and rings whose IRQ
    was released when their owner was abandoned are not visible there; for
    those the kernel log (kernel_log) is the evidence, and configfs
    gpu_quarantine shows quarantined contexts whose group still exists.

    Everything that could not be checked is listed in `unknown`; a preflight
    (--require-budget) must fail on it, never pass by default: unreadable
    /proc/interrupts or configfs, a kernel log that was not read or failed,
    and busy_poll streams whose openers (`openers`, see stream_openers) could
    not all be inspected.
    """
    unknown = []
    configfs = Path(configfs) if configfs else root / 'kernel/config/thunderbolt/stream'
    domains = {}

    def slot(domain):
        return domains.setdefault(domain, dict(streams=[], usb4net_services=[], quarantined=[],
                                               busy_poll=[]))

    try:
        groups = sorted(configfs.iterdir()) if configfs.is_dir() else []
    except OSError as exc:
        groups = []
        unknown.append(f'cannot list {configfs}: {exc.strerror or exc}')
    for group in groups:
        match = SERVICE.match(group.name)
        if not match:
            continue
        try:
            names = sorted(p.name for p in group.iterdir() if p.is_dir())
        except FileNotFoundError:
            continue                    # removed meanwhile
        except OSError as exc:
            unknown.append(f'cannot list {group}: {exc.strerror or exc}')
            continue
        item = slot(int(match[1]))
        for name in names:
            key = f'{group.name}/{name}'
            item['streams'].append(key)
            if read(group / name / 'gpu_quarantine') == '1':
                item['quarantined'].append(key)
            if read(group / name / 'busy_poll') == '1':
                index = read(group / name / 'index')
                item['busy_poll'].append((key, int(index) if index and index.isdecimal() else None))
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
    try:
        held = held_rings(Path(interrupts).read_text())
        held_error = None
    except OSError as exc:
        held, held_error = None, f'{interrupts}: {exc.strerror or exc}'
    nhis = nhi_by_domain(root)
    for domain in nhis:
        slot(domain)
    problems, notes = [], []
    for domain, item in sorted(domains.items()):
        item['budget'] = LAB_DATA_RING_PAIRS
        item['nhi'] = nhis.get(domain)
        item['planned'] = len(item['streams']) + len(item['usb4net_services'])
        rings = held.get(item['nhi'], 0) if held is not None and item['nhi'] else None
        item['held_data_rings'] = rings
        item['held_pairs'] = None if rings is None else (rings + 1) // 2
        if item['usb4net_services']:
            problems.append(f'domain {domain}: thunderbolt-net bound to '
                            f'{", ".join(item["usb4net_services"])} takes a ring pair')
        if item['planned'] > item['budget']:
            problems.append(f'domain {domain}: {item["planned"]} ring-pair consumers for a budget '
                            f'of {item["budget"]}; some stream opens will fail')
        for key in item['quarantined']:
            problems.append(f'domain {domain}: {key} has a quarantined GPU context that keeps '
                            'its rings (gpu_recover reap/reset)')
        if item['held_pairs']:
            if item['held_pairs'] > item['planned']:
                problems.append(f'domain {domain}: {item["held_data_rings"]} data rings held but '
                                f'only {item["planned"]} configured consumers: a GPU context '
                                'retained after close or rmdir (orphan; reap with '
                                'thunderbolt_stream.gpu_reap_orphans) or another ring owner')
            elif not allow_held:
                problems.append(f'domain {domain}: {item["held_data_rings"]} data rings already '
                                'held (open streams, USB4NET or a retained GPU context); close '
                                'them before a test')
        for key, index in item['busy_poll']:
            # No interrupt: an open busy_poll stream holds rings that
            # /proc/interrupts cannot show, so look for its openers instead.
            if openers is None:
                unknown.append(f'domain {domain}: busy_poll stream {key} may hold rings without '
                               'an interrupt and its openers were not checked')
                continue
            pids, unreadable = openers[0].get(index, []) if index is not None else [], openers[1]
            if index is None:
                unknown.append(f'domain {domain}: busy_poll stream {key} has no readable index')
            elif pids and not allow_held:
                problems.append(f'domain {domain}: busy_poll stream {key} (/dev/tbstream{index}) '
                                f'is open by pid {", ".join(map(str, pids))} and holds rings '
                                'without an interrupt')
            elif not pids and unreadable:
                unknown.append(f'domain {domain}: busy_poll stream {key} may be open: '
                               f'{unreadable} processes could not be inspected (run as root)')
        item['busy_poll'] = [key for key, _index in item['busy_poll']]
    if log_error:
        unknown.append(f'kernel log unavailable ({log_error}): rings of abandoned owners and '
                       'orphaned GPU contexts reported only there were not checked')
    elif kernel_log is None:
        unknown.append('kernel log not read: rings of abandoned owners and orphaned GPU '
                       'contexts reported only there were not checked')
    for line in kernel_log or []:
        for needle, label in EVIDENCE:
            if needle in line:
                (notes if label == 'teardown reset pending' else problems).append(
                    f'kernel log: {label}: {line.strip()}')
    if held_error:
        unknown.append(f'held rings unknown ({held_error})')
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
                modprobe_policy_present=policy.is_file(), problems=problems, notes=notes,
                unknown=unknown,
                limits='held rings come from MSI-X vectors in /proc/interrupts: rings without '
                       'interrupts (busy_poll) and rings of abandoned owners (IRQ released) '
                       'are only visible through the kernel log; busy_poll streams are '
                       'checked through their openers in /proc/<pid>/fd')


def filter_log(text):
    return [line for line in text.splitlines()
            if re.search(r'thunderbolt|tbstream|usb4|lane bonding|\bnhi\b', line, re.I)]


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
                        help='exit 1 if USB4NET, held rings or extra consumers take the lab ring '
                             'pairs (run before every benchmark)')
    parser.add_argument('--allow-held', action='store_true',
                        help='do not flag rings held by configured consumers (monitoring a run)')
    parser.add_argument('--interrupts', type=Path, default=Path('/proc/interrupts'))
    parser.add_argument('--proc-root', type=Path, default=Path('/proc'))
    parser.add_argument('--kernel-log', type=Path,
                        help='read the kernel log from FILE instead of journalctl (offline/tests)')
    parser.add_argument('--registers', action='store_true',
                        help='also read LANE_ADP_CS_0/1 of both lane adapters of every USB4 port '
                             'and sideband LINK_CONF/TXFFE from debugfs (root; read-only) and '
                             'compare them with sysfs; exit 1 on a mismatch')
    parser.add_argument('--debugfs-root', type=Path, default=Path('/sys/kernel/debug/thunderbolt'))
    parser.add_argument('--lanecap', action='store_true',
                        help='with --registers, print the lab lanecap text instead of JSON')
    args = parser.parse_args(argv)
    report = collect(args.sysfs_root)
    if args.registers or args.lanecap:
        report['lanes'] = lane_registers(args.sysfs_root, args.debugfs_root)
    if args.kernel_log is not None:
        try:
            report['kernel_log'] = filter_log(args.kernel_log.read_text(errors='replace'))
        except OSError as exc:
            report['log_warning'] = f'{args.kernel_log}: {exc.strerror or exc}'
    elif args.logs or args.require_budget:
        # The whole boot: an orphan or abandoned-ring message can be old.
        try:
            result = subprocess.run(['journalctl', '-k', '-b', '--no-pager'],
                                    text=True, capture_output=True, timeout=30)
            report['kernel_log'] = filter_log(result.stdout)
            if result.returncode or result.stderr.strip():
                report['log_warning'] = result.stderr.strip() or 'journalctl failed'
        except (OSError, subprocess.TimeoutExpired) as exc:
            report['log_warning'] = str(exc)
    report['ring_budget'] = ring_budget(args.sysfs_root, modprobe_dir=args.modprobe_dir,
                                        interrupts=args.interrupts,
                                        kernel_log=report.get('kernel_log'),
                                        allow_held=args.allow_held,
                                        log_error=report.get('log_warning'),
                                        openers=stream_openers(args.proc_root))
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
    budget = report['ring_budget']
    if args.require_budget and (budget['problems'] or budget['unknown']):
        # Fail closed: anything that could not be checked blocks the preflight.
        for problem in budget['problems']:
            print(f'ring budget: {problem}', file=sys.stderr)
        for unknown in budget['unknown']:
            print(f'ring budget: not verified: {unknown}', file=sys.stderr)
        return 1
    return status

if __name__ == '__main__':
    sys.exit(main())
