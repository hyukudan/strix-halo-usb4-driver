#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Read-only USB4STREAM topology report (installed as usb4stream-describe).

Builds the /run/usb4stream-layout.json document: the original per-stream
entries ("<service>/<stream>" -> device, configfs), now enriched with the
local controller, port, link speed and remote host of every stream, plus a
`version` and a `topology` section that groups the streams by cable.

Nothing is written to sysfs, configfs or modules, and no /dev/tbstreamN is
opened: open() allocates rings and enables paths, and the last close() sends
a CLOSE packet to the peer, which would disturb a running session. Whether a
stream is "openable" is therefore derived (device node present, caller has
read/write access, service attached to thunderbolt_stream).

With --merge it combines the reports of the two ends of a link (one file per
host) into a pair view: which local controller talks to which remote one,
whether the wiring is straight or crossed, and which tbstreamN pairs with
which.
"""
import argparse
import datetime
import json
import os
from pathlib import Path
import re
import socket
import stat
import subprocess
import sys

VERSION = 2
TOOL = 'usb4stream-describe'
NHI_CLASS = '0x0c0340'          # PCI class of a USB4 host interface
FULL_LANE_GBPS = 20.0           # Gen3 lane
FULL_LANES = 2
SPEED = re.compile(r'^\s*(\d+(?:\.\d+)?)\s*Gb/s\s*$')
DEVICE_NAME = re.compile(r'^(\d+)-([0-9a-f]+)$')
SERVICE_NAME = re.compile(r'^(\d+)-([0-9a-f]+)\.(\d+)$')
STREAM_ATTRS_INT = ('index', 'in_hopid', 'out_hopid')


def read(path):
    try:
        return path.read_text().strip()
    except (OSError, UnicodeDecodeError):
        return None


def as_int(text):
    try:
        return int(text, 0)
    except (TypeError, ValueError):
        return None


def driver_name(path):
    """Basename of a driver symlink; None if absent or removed meanwhile."""
    try:
        return os.path.basename(os.readlink(path / 'driver'))
    except OSError:
        return None


def listdir(path):
    """Sorted children; empty if the directory vanished during enumeration."""
    try:
        return sorted(path.iterdir())
    except OSError:
        return []


def uevent(path):
    out = {}
    for line in (read(path / 'uevent') or '').splitlines():
        key, _, value = line.partition('=')
        out[key] = value
    return out


def link_direction(device, which):
    raw_speed = read(device / f'{which}_speed')
    raw_lanes = read(device / f'{which}_lanes')
    match = SPEED.fullmatch(raw_speed or '')
    speed = float(match[1]) if match else None
    lanes = as_int(raw_lanes)
    total = speed * lanes if speed is not None and lanes in (1, 2) else None
    return dict(gbps_per_lane=speed, lanes=lanes, gbps=total,
                raw_speed=raw_speed, raw_lanes=raw_lanes)


def link_of(device):
    rx, tx = link_direction(device, 'rx'), link_direction(device, 'tx')
    totals = [d['gbps'] for d in (rx, tx)]
    gbps = min(totals) if None not in totals else None
    if any(d['gbps_per_lane'] is None or d['lanes'] not in (1, 2) for d in (rx, tx)):
        full = None                 # unreadable/unknown: neither full nor degraded
    else:
        full = all(d['gbps_per_lane'] == FULL_LANE_GBPS and d['lanes'] == FULL_LANES
                   for d in (rx, tx))
    return dict(rx=rx, tx=tx, gbps=gbps, full_speed=full,
                note='sysfs speed is per lane; gbps = per-lane speed x lanes, '
                     'minimum of both directions; raw link rate, not throughput')


def fmt_gbps(value):
    if value is None:
        return '?'
    return f'{value:g}'


class Describer:
    def __init__(self, sysfs_root=Path('/sys'), configfs_root=None,
                 dev_root=Path('/dev'), layout_path=None, port_labels=None,
                 hostname=None, require_chardev=True, kernel_log=None):
        self.sysfs = Path(sysfs_root)
        self.configfs = Path(configfs_root) if configfs_root else \
            self.sysfs / 'kernel/config/thunderbolt/stream'
        self.dev = Path(dev_root)
        self.layout_path = Path(layout_path) if layout_path else None
        self.labels = port_labels or {}
        self.hostname = hostname or socket.gethostname()
        self.require_chardev = require_chardev
        self.kernel_log = kernel_log or []
        self.warnings = []

    # -- sysfs inventory -------------------------------------------------
    def controllers(self):
        out = []
        base = self.sysfs / 'bus/pci/devices'
        for pci in listdir(base):
            if read(pci / 'class') != NHI_CLASS:
                continue
            vendor, device = read(pci / 'vendor'), read(pci / 'device')
            driver = driver_name(pci)
            domains = sorted(int(d.name[6:]) for d in listdir(pci)
                             if d.name.startswith('domain') and d.name[6:].isdecimal())
            item = dict(pci=pci.name,
                        device_id=f'{(vendor or "?")[2:]}:{(device or "?")[2:]}',
                        acpi_path=read(pci / 'firmware_node/path'),
                        driver=driver, domain=domains[0] if domains else None)
            if domains:
                item['state'] = 'up'
                router = pci / f'domain{domains[0]}' / f'{domains[0]}-0'
                item['host_router'] = router.name
                item['host_router_uuid'] = read(router / 'unique_id')
                item['generation'] = as_int(read(router / 'generation'))
                item['usb4_ports'] = sorted(
                    int(p.name[9:]) for p in listdir(router)
                    if p.name.startswith('usb4_port') and p.name[9:].isdecimal())
                item['port_link'] = {str(p): read(router / f'usb4_port{p}/link')
                                     for p in item['usb4_ports']}
            elif driver is None:
                item['state'] = 'controller-down'
                item['reason'] = ('no driver bound: probe failed or thunderbolt '
                                  'not loaded (see kernel log)')
            else:
                item['state'] = 'controller-down'
                item['reason'] = f'bound to {driver} but no Thunderbolt domain'
            item['port_label'] = self.label_for(item)
            if item['state'] != 'up':
                hits = [l for l in self.kernel_log if pci.name in l and 'probe' in l]
                if hits:
                    item['kernel_log'] = hits[-1]
            out.append(item)
        return out

    def label_for(self, ctrl):
        for key in (ctrl['pci'], ctrl['device_id'], ctrl.get('acpi_path')):
            if key and key in self.labels:
                return self.labels[key]
        return None

    def tb_devices(self):
        xdomains, services = {}, {}
        base = self.sysfs / 'bus/thunderbolt/devices'
        for dev in listdir(base):
            kind = uevent(dev).get('DEVTYPE')
            if kind == 'thunderbolt_xdomain':
                match = DEVICE_NAME.fullmatch(dev.name)
                if not match:
                    continue
                route = int(match[2], 16)
                hops = []
                while route:
                    hops.append(route & 0xff)
                    route >>= 8
                xdomains[dev.name] = dict(
                    xdomain=dev.name, domain=int(match[1]), route=f'0x{match[2]}',
                    first_hop_port=hops[0] if hops else None, hops=len(hops),
                    device_name=read(dev / 'device_name'),
                    vendor_name=read(dev / 'vendor_name'),
                    unique_id=read(dev / 'unique_id'),
                    maxhopid=as_int(read(dev / 'maxhopid')),
                    link=link_of(dev))
            elif kind == 'thunderbolt_service':
                services[dev.name] = dict(key=read(dev / 'key'), driver=driver_name(dev))
        return xdomains, services

    # -- streams ---------------------------------------------------------
    def stream_dirs(self):
        """[(service, stream, configfs_dir_or_None, legacy_entry)]"""
        found = []
        if self.configfs.is_dir():
            try:
                services = sorted(self.configfs.iterdir())
            except OSError as exc:
                services = []
                self.warnings.append(f'cannot list {self.configfs}: {exc}')
            # Groups can be removed (rmdir, unplug) while we walk: skip, do not crash.
            for svc in services:
                for st in listdir(svc):
                    if st.is_dir():
                        found.append((svc.name, st.name, st, None))
        if not found and self.layout_path and self.layout_path.exists():
            try:
                old = json.loads(self.layout_path.read_text())
            except (OSError, ValueError) as exc:
                old = {}
                self.warnings.append(f'cannot read {self.layout_path}: {exc}')
            for key, entry in old.items():
                if '/' in key and isinstance(entry, dict):
                    svc, _, st = key.partition('/')
                    found.append((svc, st, None, entry))
            if found:
                self.warnings.append('configfs not readable; streams taken from '
                                     f'{self.layout_path}')
        return found

    def device_state(self, index):
        if index is None:
            return False, False, 'stream index unknown'
        node = self.dev / f'tbstream{index}'
        try:
            mode = node.stat().st_mode
        except OSError:
            return False, False, f'{node} does not exist'
        if self.require_chardev and not stat.S_ISCHR(mode):
            return False, False, f'{node} is not a character device'
        if not os.access(node, os.R_OK | os.W_OK):
            uid = os.getuid() if hasattr(os, 'getuid') else '?'
            return True, False, f'no read/write permission on {node} for uid {uid}'
        return True, True, None

    # -- report ----------------------------------------------------------
    def inventory(self):
        """What a hotplug can change: Thunderbolt devices and their drivers, PCI
        domains of the NHIs, configfs groups. Compared before/after a pass."""
        items = []
        devices = self.sysfs / 'bus/thunderbolt/devices'
        for dev in listdir(devices):
            items.append(('tb', dev.name, driver_name(dev)))
        for pci in listdir(self.sysfs / 'bus/pci/devices'):
            for child in listdir(pci):
                if child.name.startswith('domain'):
                    items.append(('pci', pci.name, child.name))
        for svc in listdir(self.configfs):
            for st in listdir(svc):
                items.append(('cfg', svc.name, st.name))
        return items

    def describe_stable(self, passes):
        """describe() until the inventory is unchanged across one pass (at most `passes`)."""
        initial = list(self.warnings)       # e.g. a journalctl failure
        for attempt in range(1, passes + 1):
            before = self.inventory()
            self.warnings = list(initial)
            layout = self.describe()
            if self.inventory() == before:
                layout['topology']['passes'] = attempt
                return layout
        layout['topology']['passes'] = passes
        layout['topology']['warnings'].append(
            f'topology changed during each of {passes} passes; this layout may already be '
            'stale (a later hotplug refresh will rewrite it)')
        return layout

    def describe(self):
        ctrls = self.controllers()
        by_domain = {c['domain']: c for c in ctrls if c['domain'] is not None}
        xdomains, services = self.tb_devices()

        # One cable slot per usb4 port of an up controller, one per down one.
        cables = []
        for ctrl in ctrls:
            local = {k: ctrl.get(k) for k in ('pci', 'device_id', 'acpi_path', 'domain',
                                              'port_label', 'host_router_uuid')}
            if ctrl['state'] != 'up':
                cables.append(dict(key=f'{ctrl["pci"]}/usb4_port?',
                                   local=dict(local, usb4_port=None),
                                   state='controller-down', reason=ctrl.get('reason'),
                                   remote=None, link=None))
                continue
            ports = ctrl['usb4_ports'] or [None]
            for port in ports:
                peers = [x for x in xdomains.values()
                         if x['domain'] == ctrl['domain'] and
                         (port is None or x['first_hop_port'] == port)]
                cable = dict(key=f'{ctrl["pci"]}/usb4_port{port if port is not None else "?"}',
                             local=dict(local, usb4_port=port), remote=None, link=None)
                if peers:
                    xd = peers[0]
                    cable['state'] = 'up'
                    cable['remote'] = {k: xd[k] for k in ('xdomain', 'route', 'hops',
                                                          'device_name', 'vendor_name',
                                                          'unique_id', 'maxhopid')}
                    cable['link'] = xd['link']
                    if len(peers) > 1:
                        cable['other_remotes'] = [p['xdomain'] for p in peers[1:]]
                else:
                    cable['state'] = 'no-peer'
                    cable['reason'] = ('controller up but no XDomain on this port: '
                                       'cable unplugged, or the far-end controller is down')
                cables.append(cable)
        for letter, cable in zip('ABCDEFGHIJKLMNOPQRSTUVWXYZ', cables):
            cable['id'] = letter
            cable['streams'], cable['devices'] = [], []

        def cable_for(domain, port):
            for cable in cables:
                loc = cable['local']
                if loc['domain'] == domain and loc['usb4_port'] in (port, None):
                    return cable
            return None

        layout = {}
        for svc, st, cdir, legacy in self.stream_dirs():
            attrs = {}
            if cdir is not None:
                for f in listdir(cdir):
                    value = read(f)
                    if value is not None:
                        attrs[f.name] = as_int(value) if as_int(value) is not None else value
                if 'index' not in attrs:
                    # The group disappeared between listing and reading it.
                    self.warnings.append(f'{svc}/{st} vanished during enumeration; skipped')
                    continue
            index = attrs.get('index')
            if index is None and legacy:
                m = re.search(r'(\d+)$', legacy.get('device', ''))
                index = int(m[1]) if m else None
            key = f'{svc}/{st}'
            entry = dict(device=legacy['device'] if legacy else f'/dev/tbstream{index}',
                         configfs=legacy['configfs'] if legacy else str(cdir))
            entry.update(service=svc, stream=st, index=index,
                         in_hopid=attrs.get('in_hopid'), out_hopid=attrs.get('out_hopid'),
                         config={k: v for k, v in attrs.items() if k not in STREAM_ATTRS_INT})
            node_ok, access_ok, why = self.device_state(index)
            svc_info = services.get(svc)
            attached = bool(svc_info and svc_info['driver'] == 'thunderbolt_stream')
            # open() fails with EINVAL unless both HopIDs are > 0 (tbstream_dev_valid).
            hops = (attrs.get('in_hopid'), attrs.get('out_hopid'))
            hops_ok = all(isinstance(h, int) and h > 0 for h in hops)
            entry['exists'] = (cdir is not None or legacy is not None) and node_ok
            entry['attached'] = attached
            entry['openable'] = entry['exists'] and access_ok and attached and hops_ok
            if not entry['openable']:
                if why is None and not attached:
                    why = (f'service {svc} not present in sysfs (no peer on that domain/port)'
                           if not svc_info else f'service {svc} bound to {svc_info["driver"]}')
                if why is None and not hops_ok:
                    why = (f'HopIDs not allocated (in {hops[0]}, out {hops[1]}): '
                           'open() rejects HopIDs <= 0' if cdir is not None else
                           'HopIDs unknown (configfs not readable)')
                entry['openable_reason'] = why
            entry['open_checked'] = False

            match = SERVICE_NAME.fullmatch(svc)
            entry['local'] = entry['link'] = entry['remote'] = entry['cable'] = None
            if match:
                domain, xd_name = int(match[1]), f'{match[1]}-{match[2]}'
                route = int(match[2], 16)
                port = route & 0xff if route else None
                ctrl = by_domain.get(domain)
                if ctrl:
                    entry['local'] = dict(nhi_pci=ctrl['pci'], nhi_device_id=ctrl['device_id'],
                                          nhi_acpi_path=ctrl['acpi_path'], domain=domain,
                                          usb4_port=port, port_label=ctrl['port_label'])
                    cable = cable_for(domain, port)
                    if cable:
                        entry['cable'] = cable['id']
                        cable['streams'].append(key)
                        cable['devices'].append(entry['device'])
                else:
                    entry['local_reason'] = (f'domain {domain} does not exist: its controller '
                                             'is down or was numbered differently at probe')
                    self.warnings.append(f'{key} ({entry["device"]}) is configured on domain '
                                         f'{domain}, which does not exist on this boot')
                xd = xdomains.get(xd_name)
                if xd:
                    entry['link'] = xd['link']
                    entry['remote'] = {k: xd[k] for k in ('xdomain', 'route', 'device_name',
                                                          'vendor_name', 'unique_id')}
            layout[key] = entry

        for cable in cables:
            cable['streams_openable'] = sum(1 for k in cable['streams'] if layout[k]['openable'])

        # Warnings that a consumer should surface.
        down = [c for c in ctrls if c['state'] != 'up']
        for c in down:
            self.warnings.append(f'controller {c["pci"]} ({c["device_id"]}) is DOWN: '
                                 f'{c.get("reason")}' +
                                 (f' [{c["kernel_log"]}]' if c.get('kernel_log') else ''))
        if down and by_domain:
            mapping = ', '.join(f'domain{d}={c["pci"]}' for d, c in sorted(by_domain.items()))
            self.warnings.append('domain numbers are assigned at probe time and shift when a '
                                 f'controller is down ({mapping}); configfs service names '
                                 '(D-R.S) follow the domain, not the physical port')
        for cable in cables:
            link = cable['link']
            if cable['state'] == 'no-peer':
                self.warnings.append(f'cable {cable["id"]} ({cable["key"]}) has no peer: '
                                     'cable unplugged or the far-end controller is down')
            if cable['state'] == 'up' and link and link['full_speed'] is None:
                self.warnings.append(
                    f'cable {cable["id"]} ({cable["key"]}) link speed unknown: '
                    f'rx {link["rx"]["raw_speed"]!r} x {link["rx"]["raw_lanes"]!r}, '
                    f'tx {link["tx"]["raw_speed"]!r} x {link["tx"]["raw_lanes"]!r}')
            if cable['state'] == 'up' and link and link['full_speed'] is False:
                self.warnings.append(
                    f'cable {cable["id"]} ({cable["key"]}) trained below Gen3 x2: '
                    f'rx {fmt_gbps(link["rx"]["gbps_per_lane"])}x{link["rx"]["lanes"]}, '
                    f'tx {fmt_gbps(link["tx"]["gbps_per_lane"])}x{link["tx"]["lanes"]}')

        peers = {}
        for cable in cables:
            if cable['state'] == 'up':
                name = cable['remote']['device_name'] or cable['remote']['unique_id']
                peer = peers.setdefault(name, dict(unique_ids=[], cables=[], gbps=0.0))
                peer['cables'].append(cable['id'])
                if cable['remote']['unique_id'] not in peer['unique_ids']:
                    peer['unique_ids'].append(cable['remote']['unique_id'])
                peer['gbps'] += cable['link']['gbps'] or 0.0

        up = [c for c in cables if c['state'] == 'up']
        parts = []
        for c in cables:
            where = f'{c["local"]["pci"]}' + (f' port{c["local"]["usb4_port"]}'
                                               if c['local']['usb4_port'] is not None else '')
            if c['local']['port_label']:
                where += f' ({c["local"]["port_label"]})'
            if c['state'] == 'up':
                l = c['link']
                parts.append(f'{c["id"]} {where} -> {c["remote"]["device_name"]} '
                             f'{fmt_gbps(l["gbps"])} Gb/s ({fmt_gbps(l["rx"]["gbps_per_lane"])} Gb/s '
                             f'x{l["rx"]["lanes"]} lanes), {c["streams_openable"]}/'
                             f'{len(c["streams"])} streams openable')
            else:
                parts.append(f'{c["id"]} {where} {c["state"].upper()}')
        summary = (f'{self.hostname}: {len(cables)} cable slot(s), {len(up)} up; ' +
                   '; '.join(parts))

        layout['version'] = VERSION
        layout['topology'] = dict(
            host=self.hostname, tool=f'{TOOL} {VERSION}',
            generated_at=datetime.datetime.now(datetime.timezone.utc).isoformat(timespec='seconds'),
            summary=summary, cables_total=len(cables), cables_up=len(up),
            gbps_total=sum(c['link']['gbps'] or 0.0 for c in up),
            controllers=ctrls, cables=cables, peers=peers,
            warnings=self.warnings,
            notes=['stream entries are the top-level keys containing "/"; '
                   '"version" and "topology" are the only other keys',
                   'openable is derived without opening the device (open starts rings, '
                   'close sends CLOSE to the peer)',
                   'streams pair across hosts by stream name on the same cable '
                   '(dataN <-> dataN); use --merge or the engine handshake to resolve '
                   'which remote tbstreamN that is'])
        return layout


# -- pair view ---------------------------------------------------------------
def merge(reports):
    hosts = {}
    for rep in reports:
        host = rep.get('topology', {}).get('host') if isinstance(rep, dict) else None
        if not host:
            raise ValueError('not a usb4stream-describe report (no topology.host)')
        if host in hosts:
            raise ValueError(f'duplicate host {host!r}: each report must come from a '
                             'different node (check --hostname)')
        hosts[host] = rep
    uuid_owner = {}
    for host, rep in hosts.items():
        for ctrl in rep['topology']['controllers']:
            if ctrl.get('host_router_uuid'):
                uuid_owner[ctrl['host_router_uuid']] = (host, ctrl['pci'])

    def cable_by_pci(host, pci):
        for cable in hosts[host]['topology']['cables']:
            if cable['local']['pci'] == pci:
                return cable
        return None

    links, warnings, seen = [], [], set()
    for host, rep in hosts.items():
        for cable in rep['topology']['cables']:
            if cable['state'] != 'up':
                continue
            owner = uuid_owner.get(cable['remote']['unique_id'])
            if not owner:
                warnings.append(f'{host} cable {cable["id"]} reaches unknown host '
                                f'{cable["remote"]["device_name"]} {cable["remote"]["unique_id"]}')
                continue
            other = cable_by_pci(*owner)
            pair = tuple(sorted([(host, cable['key']), (owner[0], other['key'])]))
            if pair in seen:
                continue
            seen.add(pair)
            reciprocal = (other['state'] == 'up' and
                          other['remote']['unique_id'] == cable['local']['host_router_uuid'])
            ends = [(host, cable), (owner[0], other)]
            same = cable['local']['device_id'] == other['local']['device_id'] and \
                cable['local']['acpi_path'] == other['local']['acpi_path']
            labels = [c['local']['port_label'] for _, c in ends]
            gbps = [c['link']['gbps'] for _, c in ends if c.get('link')]
            link = dict(
                ends=[dict(host=h, cable=c['id'], nhi_pci=c['local']['pci'],
                           nhi_device_id=c['local']['device_id'],
                           nhi_acpi_path=c['local']['acpi_path'],
                           usb4_port=c['local']['usb4_port'], port_label=c['local']['port_label'],
                           state=c['state']) for h, c in ends],
                state='up' if reciprocal else 'half-up',
                wiring='straight' if same else 'crossed',
                wiring_by_label='-'.join(labels) if all(labels) else None,
                gbps=min(gbps) if gbps and None not in gbps else None,
                stream_pairs=[])
            if not reciprocal:
                warnings.append(f'link {host}:{cable["id"]} <-> {owner[0]}:{other["id"]} '
                                f'is only seen from {host} (far end state {other["state"]})')
            if len(set(gbps)) > 1:
                warnings.append(f'link {host}:{cable["id"]} reports different speeds per end: {gbps}')
            for key in cable['streams']:
                a = rep[key]
                for okey in other['streams']:
                    b = hosts[owner[0]][okey]
                    if b['stream'] == a['stream']:
                        link['stream_pairs'].append(dict(
                            stream=a['stream'],
                            a=dict(host=host, key=key, device=a['device'], openable=a['openable']),
                            b=dict(host=owner[0], key=okey, device=b['device'],
                                   openable=b['openable'])))
            links.append(link)

    # Dead cables cannot be seen; pair the leftovers when it is unambiguous.
    leftovers = {h: [c for c in r['topology']['cables'] if c['state'] != 'up']
                 for h, r in hosts.items()}
    suspected = []
    if len(hosts) == 2:
        (h1, l1), (h2, l2) = leftovers.items()
        if len(l1) == 1 and len(l2) == 1:
            a, b = l1[0], l2[0]
            same = a['local']['device_id'] == b['local']['device_id']
            suspected.append(dict(
                ends=[dict(host=h1, cable=a['id'], nhi_pci=a['local']['pci'], state=a['state']),
                      dict(host=h2, cable=b['id'], nhi_pci=b['local']['pci'], state=b['state'])],
                state='down', inferred=True,
                wiring='straight' if same else 'crossed',
                note='inferred from the only unconnected slot on each host'))
    for h, items in leftovers.items():
        for c in items:
            warnings.append(f'{h} cable {c["id"]} ({c["key"]}) is {c["state"]}')

    wirings = sorted({l['wiring'] + (f' ({l["wiring_by_label"]})' if l['wiring_by_label'] else '')
                      for l in links})
    summary = (f'{len(links)} live link(s) between {", ".join(sorted(hosts))}; '
               f'wiring {", ".join(wirings) or "n/a"}; ' +
               '; '.join(f'{l["ends"][0]["host"]}:{l["ends"][0]["nhi_pci"]} <-> '
                         f'{l["ends"][1]["host"]}:{l["ends"][1]["nhi_pci"]} '
                         f'{fmt_gbps(l["gbps"])} Gb/s {l["state"]}' for l in links) +
               (f'; {len(suspected)} dead link(s) inferred' if suspected else ''))
    return dict(version=VERSION, kind='pair-topology', hosts=sorted(hosts), summary=summary,
                links=links, suspected_down_links=suspected, warnings=warnings)


def load_labels(path, explicit):
    if path is None or not Path(path).exists():
        if explicit:
            raise SystemExit(f'{TOOL}: port label file {path} not found')
        return {}
    data = json.loads(Path(path).read_text())
    if not isinstance(data, dict) or not all(isinstance(v, str) for v in data.values()):
        raise SystemExit(f'{TOOL}: {path} must map PCI address/ID/ACPI path to a label string')
    return data


def read_kernel_log():
    """(thunderbolt lines, error or None): a failed journalctl is reported, not hidden."""
    try:
        result = subprocess.run(['journalctl', '-k', '-b', '--no-pager', '-n', '5000'],
                                text=True, capture_output=True, timeout=15)
    except (OSError, subprocess.TimeoutExpired) as exc:
        return [], f'kernel log unavailable: {exc}'
    lines = [l for l in result.stdout.splitlines() if 'thunderbolt' in l]
    if result.returncode:
        detail = result.stderr.strip() or f'exit status {result.returncode}'
        return lines, f'kernel log unavailable: journalctl failed: {detail}'
    return lines, None


def write_atomic(path, text):
    path = Path(path)
    tmp = path.with_name(f'.{path.name}.{os.getpid()}.tmp')
    tmp.write_text(text)
    os.replace(tmp, path)


def main(argv=None):
    p = argparse.ArgumentParser(prog=TOOL, description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--sysfs-root', type=Path, default=Path('/sys'))
    p.add_argument('--configfs-root', type=Path,
                   help='default: <sysfs-root>/kernel/config/thunderbolt/stream')
    p.add_argument('--dev-root', type=Path, default=Path('/dev'))
    p.add_argument('--layout', type=Path, default=Path('/run/usb4stream-layout.json'),
                   help='fallback source of streams when configfs is not readable')
    p.add_argument('--port-labels', type=Path,
                   help='JSON {"0000:c7:00.5": "rear", ...} (default /etc/usb4stream-ports.json if present)')
    p.add_argument('--hostname')
    p.add_argument('--logs', action='store_true',
                   help='read the kernel log (journalctl) to explain controllers that are down')
    p.add_argument('--output', type=Path, help='write atomically instead of stdout')
    p.add_argument('--summary', action='store_true', help='print only the summary line(s)')
    p.add_argument('--until-stable', type=int, default=1, metavar='N',
                   help='repeat the scan until sysfs/configfs did not change during a pass '
                        '(at most N passes; the hotplug refresh uses 5)')
    p.add_argument('--merge', nargs='+', type=Path, metavar='REPORT',
                   help='combine per-host reports into a pair view (no sysfs access)')
    args = p.parse_args(argv)

    if args.merge:
        try:
            result = merge([json.loads(f.read_text()) for f in args.merge])
        except (OSError, ValueError) as exc:
            p.exit(2, f'{TOOL}: --merge: {exc}\n')
        summary = result['summary']
    else:
        labels = load_labels(args.port_labels or Path('/etc/usb4stream-ports.json'),
                             args.port_labels is not None)
        log, log_error = read_kernel_log() if args.logs else (None, None)
        describer = Describer(args.sysfs_root, args.configfs_root, args.dev_root, args.layout,
                              labels, args.hostname, kernel_log=log)
        if log_error:
            describer.warnings.append(log_error)
        result = describer.describe_stable(max(1, args.until_stable))
        summary = '\n'.join([result['topology']['summary']] +
                            [f'warning: {w}' for w in result['topology']['warnings']])
    text = json.dumps(result, indent=2) + '\n'
    if args.output:
        write_atomic(args.output, text)
    if args.summary:
        print(summary)
    elif not args.output:
        sys.stdout.write(text)
    return 0


if __name__ == '__main__':
    sys.exit(main())
