# SPDX-License-Identifier: GPL-2.0-only
"""usb4stream-describe against fake sysfs/configfs trees: wiring, speeds, dead links."""
import importlib.util
import io
import json
import os
from pathlib import Path
import contextlib
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('usb4stream_describe',
                                              REPO / 'tools/lab/usb4stream_describe.py')
describe_mod = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(describe_mod)

NHI = {'0000:c7:00.5': ('0x158d', r'\_SB_.PCI0.GPPC.NHI0'),
       '0000:c7:00.6': ('0x158e', r'\_SB_.PCI0.GPPC.NHI1')}
LABELS = {'0000:c7:00.5': 'rear', '0000:c7:00.6': 'front'}


def uuid(host, pci):
    return f'{host}-{pci[-1]}-router-uuid'


def write(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text + '\n')


class FakeHost:
    """One node: NHIs (up or down), XDomains on port 2, configfs streams, /dev nodes.

    `links` maps local PCI -> (remote host, remote PCI, gbps_per_lane, lanes) or
    None for an up controller without a peer. A PCI absent from `up` failed probe.
    """

    def __init__(self, root, name, up, links):
        self.name, self.root = name, Path(root) / name
        self.sys, self.dev = self.root / 'sys', self.root / 'dev'
        pcis = self.sys / 'bus/pci/devices'
        tbs = self.sys / 'bus/thunderbolt/devices'
        (self.sys / 'bus/thunderbolt/drivers/thunderbolt_stream').mkdir(parents=True)
        (self.sys / 'bus/pci/drivers/thunderbolt').mkdir(parents=True)
        domain = 0
        attached = []
        for pci, (device, acpi) in NHI.items():
            node = pcis / pci
            write(node / 'class', '0x0c0340')
            write(node / 'vendor', '0x1022')
            write(node / 'device', device)
            write(node / 'firmware_node/path', acpi)
            if pci not in up:
                continue          # probe failed: no driver, no domain
            os.symlink(self.sys / 'bus/pci/drivers/thunderbolt', node / 'driver')
            router = node / f'domain{domain}' / f'{domain}-0'
            write(router / 'unique_id', uuid(name, pci))
            write(router / 'generation', '4')
            write(router / 'usb4_port2/link', 'usb4')
            link = links.get(pci)
            if link:
                rhost, rpci, speed, lanes = link
                xd = tbs / f'{domain}-2'
                write(xd / 'uevent', 'DEVTYPE=thunderbolt_xdomain')
                write(xd / 'device_name', rhost)
                write(xd / 'vendor_name', 'Linux')
                write(xd / 'unique_id', uuid(rhost, rpci))
                write(xd / 'maxhopid', '16')
                for d in ('rx', 'tx'):
                    write(xd / f'{d}_speed', f'{speed:.1f} Gb/s')
                    write(xd / f'{d}_lanes', str(lanes))
                svc = tbs / f'{domain}-2.0'
                write(svc / 'uevent', 'DEVTYPE=thunderbolt_service\nDRIVER=thunderbolt_stream')
                write(svc / 'key', 'stream')
                os.symlink(self.sys / 'bus/thunderbolt/drivers/thunderbolt_stream', svc / 'driver')
                attached.append(f'{domain}-2.0')
            domain += 1
        # Same order and attributes as setup-usb4stream-lab.
        self.configfs = self.sys / 'kernel/config/thunderbolt/stream'
        self.legacy = {}
        index = 0
        for stream_no, stream in enumerate(('data0', 'data1')):
            for svc in ('0-2.0', '1-2.0'):
                cdir = self.configfs / svc / stream
                hop = str(8 + stream_no) if svc in attached else '-1'
                for key, value in (('ring_size', '1024'), ('throttling', '8192'),
                                   ('busy_poll', '0'), ('gpu_direct', '0'),
                                   ('in_hopid', hop), ('out_hopid', hop), ('index', str(index))):
                    write(cdir / key, value)
                write(self.dev / f'tbstream{index}', '')
                self.legacy[f'{svc}/{stream}'] = {'device': f'/dev/tbstream{index}',
                                                  'configfs': str(cdir)}
                index += 1

    def describe(self, labels=None, **kw):
        return describe_mod.Describer(self.sys, dev_root=self.dev, hostname=self.name,
                                      port_labels=labels, require_chardev=False, **kw).describe()

    def snapshot(self):
        return sorted((str(p), p.is_symlink() or p.stat().st_mtime_ns,
                       p.read_bytes() if p.is_file() and not p.is_symlink() else None)
                      for p in self.root.rglob('*'))


def pair(tmp, a_up, b_up, a_links, b_links):
    return FakeHost(tmp, 'cluster1', a_up, a_links), FakeHost(tmp, 'cluster2', b_up, b_links)


S5, S6 = '0000:c7:00.5', '0000:c7:00.6'
BOTH = (S5, S6)


class DescribeTests(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory(prefix='usb4-describe-')
        self.tmp = self._tmp.name

    def tearDown(self):
        self._tmp.cleanup()

    def test_two_cables_up_straight(self):
        a, b = pair(self.tmp, BOTH, BOTH,
                    {S5: ('cluster2', S5, 20.0, 2), S6: ('cluster2', S6, 20.0, 2)},
                    {S5: ('cluster1', S5, 20.0, 2), S6: ('cluster1', S6, 20.0, 2)})
        ra, rb = a.describe(), b.describe()
        topo = ra['topology']
        self.assertEqual((topo['cables_total'], topo['cables_up'], topo['gbps_total']), (2, 2, 80.0))
        self.assertEqual([c['link']['gbps'] for c in topo['cables']], [40.0, 40.0])
        self.assertEqual(topo['peers']['cluster2']['cables'], ['A', 'B'])
        self.assertEqual(topo['warnings'], [])
        s = ra['0-2.0/data0']
        self.assertEqual((s['local']['nhi_pci'], s['local']['domain'], s['local']['usb4_port']),
                         (S5, 0, 2))
        self.assertEqual((s['remote']['device_name'], s['remote']['route'], s['remote']['unique_id']),
                         ('cluster2', '0x2', uuid('cluster2', S5)))
        self.assertEqual((s['index'], s['in_hopid'], s['out_hopid']), (0, 8, 8))
        self.assertEqual((s['link']['rx']['gbps_per_lane'], s['link']['tx']['lanes']), (20.0, 2))
        self.assertTrue(s['exists'] and s['attached'] and s['openable'])
        self.assertEqual(sorted(topo['cables'][0]['devices']), ['/dev/tbstream0', '/dev/tbstream2'])
        merged = describe_mod.merge([ra, rb])
        self.assertEqual(len(merged['links']), 2)
        self.assertEqual({l['wiring'] for l in merged['links']}, {'straight'})
        self.assertEqual({l['state'] for l in merged['links']}, {'up'})
        pairs = {(p['a']['device'], p['b']['device']) for l in merged['links'] for p in l['stream_pairs']}
        self.assertEqual(pairs, {(f'/dev/tbstream{i}', f'/dev/tbstream{i}') for i in range(4)})
        self.assertEqual(merged['suspected_down_links'], [])

    def test_crossed_wiring_with_labels(self):
        a, b = pair(self.tmp, BOTH, BOTH,
                    {S5: ('cluster2', S6, 20.0, 2), S6: ('cluster2', S5, 20.0, 2)},
                    {S5: ('cluster1', S6, 20.0, 2), S6: ('cluster1', S5, 20.0, 2)})
        ra, rb = a.describe(LABELS), b.describe(LABELS)
        self.assertEqual(ra['0-2.0/data0']['local']['port_label'], 'rear')
        merged = describe_mod.merge([ra, rb])
        self.assertEqual({l['wiring'] for l in merged['links']}, {'crossed'})
        self.assertEqual(sorted(l['wiring_by_label'] for l in merged['links']),
                         ['front-rear', 'rear-front'])
        link = next(l for l in merged['links'] if l['ends'][0]['nhi_pci'] == S5
                    and l['ends'][0]['host'] == 'cluster1')
        self.assertEqual(link['ends'][1]['nhi_pci'], S6)
        # NHI0 (domain 0, tbstream0/2) on cluster1 pairs with NHI1 (domain 1, tbstream1/3).
        self.assertEqual({(p['a']['device'], p['b']['device']) for p in link['stream_pairs']},
                         {('/dev/tbstream0', '/dev/tbstream1'), ('/dev/tbstream2', '/dev/tbstream3')})
        self.assertIn('crossed', merged['summary'])

    def test_one_cable_down_controller_failed_probe(self):
        # Today's lab state: cluster1 0000:c7:00.5 failed probe (-110).
        a, b = pair(self.tmp, (S6,), BOTH,
                    {S6: ('cluster2', S5, 20.0, 2)},
                    {S5: ('cluster1', S6, 20.0, 2), S6: None})
        ra, rb = a.describe(), b.describe()
        ta = ra['topology']
        self.assertEqual((ta['cables_total'], ta['cables_up']), (2, 1))
        self.assertEqual([(c['id'], c['state']) for c in ta['cables']],
                         [('A', 'controller-down'), ('B', 'up')])
        self.assertIsNone(ta['controllers'][0]['domain'])
        self.assertEqual(ta['controllers'][1]['domain'], 0)   # .6 became domain 0
        self.assertIn('CONTROLLER-DOWN', ta['summary'])
        self.assertTrue(any('is DOWN' in w for w in ta['warnings']))
        self.assertTrue(any('domain numbers' in w for w in ta['warnings']))
        dead = ra['1-2.0/data0']
        self.assertFalse(dead['openable'])
        self.assertFalse(dead['attached'])
        self.assertIsNone(dead['local'])
        self.assertIn('not present', dead['openable_reason'])
        live = ra['0-2.0/data1']
        self.assertEqual((live['local']['nhi_pci'], live['openable']), (S6, True))
        tb = rb['topology']
        self.assertEqual([c['state'] for c in tb['cables']], ['up', 'no-peer'])
        self.assertTrue(any('has no peer' in w for w in tb['warnings']))
        merged = describe_mod.merge([ra, rb])
        self.assertEqual(len(merged['links']), 1)
        self.assertEqual(merged['links'][0]['wiring'], 'crossed')
        self.assertEqual(len(merged['suspected_down_links']), 1)
        self.assertEqual(merged['suspected_down_links'][0]['wiring'], 'crossed')
        self.assertTrue(merged['suspected_down_links'][0]['inferred'])

    def test_kernel_log_explains_down_controller(self):
        a = FakeHost(self.tmp, 'cluster1', (S6,), {S6: ('cluster2', S5, 20.0, 2)})
        line = 'thunderbolt 0000:c7:00.5: probe with driver thunderbolt failed with error -110'
        topo = a.describe(kernel_log=['unrelated', line])['topology']
        self.assertEqual(topo['controllers'][0]['kernel_log'], line)
        self.assertTrue(any('-110' in w for w in topo['warnings']))

    def test_mixed_20_and_40_gbps(self):
        a, b = pair(self.tmp, BOTH, BOTH,
                    {S5: ('cluster2', S5, 20.0, 2), S6: ('cluster2', S6, 10.0, 2)},
                    {S5: ('cluster1', S5, 20.0, 2), S6: ('cluster1', S6, 10.0, 2)})
        ra = a.describe()
        topo = ra['topology']
        self.assertEqual([c['link']['gbps'] for c in topo['cables']], [40.0, 20.0])
        self.assertEqual([c['link']['full_speed'] for c in topo['cables']], [True, False])
        self.assertEqual(topo['gbps_total'], 60.0)
        self.assertEqual(topo['peers']['cluster2']['gbps'], 60.0)
        self.assertEqual([w for w in topo['warnings'] if 'below Gen3' in w],
                         ['cable B (0000:c7:00.6/usb4_port2) trained below Gen3 x2: rx 10x2, tx 10x2'])
        self.assertIn('20 Gb/s (10 Gb/s x2 lanes)', topo['summary'])
        merged = describe_mod.merge([ra, b.describe()])
        self.assertEqual(sorted(l['gbps'] for l in merged['links']), [20.0, 40.0])

    def test_single_lane_is_20_gbps(self):
        a = FakeHost(self.tmp, 'cluster1', BOTH, {S5: ('cluster2', S5, 20.0, 1), S6: None})
        cable = a.describe()['topology']['cables'][0]
        self.assertEqual((cable['link']['gbps'], cable['link']['full_speed']), (20.0, False))

    def test_backward_compatible_keys_and_version(self):
        a = FakeHost(self.tmp, 'cluster1', BOTH, {S5: ('cluster2', S5, 20.0, 2), S6: None})
        report = a.describe()
        self.assertEqual(report['version'], 2)
        streams = {k: v for k, v in report.items() if '/' in k}
        self.assertEqual(set(report) - set(streams), {'version', 'topology'})
        self.assertEqual({k: {f: v[f] for f in ('device', 'configfs')} for k, v in streams.items()},
                         a.legacy)

    def test_read_only_and_cli(self):
        a = FakeHost(self.tmp, 'cluster1', (S6,), {S6: ('cluster2', S5, 20.0, 2)})
        before = a.snapshot()
        out = Path(self.tmp) / 'out.json'
        with contextlib.redirect_stdout(io.StringIO()) as text:
            describe_mod.main(['--sysfs-root', str(a.sys), '--dev-root', str(a.dev),
                               '--hostname', 'cluster1', '--output', str(out), '--summary',
                               '--port-labels', str(self._labels())])
        self.assertEqual(a.snapshot(), before)
        self.assertIn('cluster1: 2 cable slot(s), 1 up', text.getvalue())
        report = json.loads(out.read_text())
        self.assertEqual(report['topology']['cables'][1]['local']['port_label'], 'front')
        # Real /dev nodes are required outside tests: plain files are not openable.
        self.assertFalse(report['0-2.0/data0']['openable'])
        self.assertIn('not a character device', report['0-2.0/data0']['openable_reason'])

    def test_layout_fallback_when_configfs_unreadable(self):
        a = FakeHost(self.tmp, 'cluster1', BOTH, {S5: ('cluster2', S5, 20.0, 2), S6: None})
        layout = Path(self.tmp) / 'layout.json'
        layout.write_text(json.dumps(a.legacy))
        report = describe_mod.Describer(a.sys, configfs_root=Path(self.tmp) / 'missing',
                                        dev_root=a.dev, layout_path=layout, hostname='x',
                                        require_chardev=False).describe()
        self.assertEqual(report['0-2.0/data1']['index'], 2)
        self.assertEqual(report['0-2.0/data1']['cable'], 'A')
        self.assertTrue(any('configfs not readable' in w for w in report['topology']['warnings']))

    def test_setup_script_uses_describe_from_its_directory(self):
        from importlib.machinery import SourceFileLoader
        path = REPO / 'tools/lab/setup-usb4stream-lab'
        loader = SourceFileLoader('setup_usb4stream_lab', str(path))
        setup = importlib.util.module_from_spec(importlib.util.spec_from_loader(loader.name, loader))
        loader.exec_module(setup)
        self.assertEqual(setup.find_describe(), str(REPO / 'tools/lab/usb4stream_describe.py'))
        self.assertEqual(setup.SERVICES, ['0-2.0', '1-2.0'])
        self.assertEqual(setup.LAYOUT, Path('/run/usb4stream-layout.json'))

    def test_hopid_zero_is_not_openable(self):
        a = FakeHost(self.tmp, 'cluster1', BOTH, {S5: ('cluster2', S5, 20.0, 2), S6: None})
        cdir = a.configfs / '0-2.0/data0'
        write(cdir / 'in_hopid', '0')
        report = a.describe()
        entry = report['0-2.0/data0']
        self.assertTrue(entry['attached'] and entry['exists'])
        self.assertFalse(entry['openable'])
        self.assertIn('HopIDs not allocated (in 0, out 8)', entry['openable_reason'])
        self.assertTrue(report['0-2.0/data1']['openable'])
        self.assertEqual(report['topology']['cables'][0]['streams_openable'], 1)

    def test_legacy_layout_without_hopids_is_not_openable(self):
        a = FakeHost(self.tmp, 'cluster1', BOTH, {S5: ('cluster2', S5, 20.0, 2), S6: None})
        layout = Path(self.tmp) / 'layout.json'
        layout.write_text(json.dumps(a.legacy))
        report = describe_mod.Describer(a.sys, configfs_root=Path(self.tmp) / 'missing',
                                        dev_root=a.dev, layout_path=layout, hostname='x',
                                        require_chardev=False).describe()
        self.assertFalse(report['0-2.0/data0']['openable'])
        self.assertIn('HopIDs unknown', report['0-2.0/data0']['openable_reason'])

    def test_unknown_speed_is_not_below_gen3(self):
        a = FakeHost(self.tmp, 'cluster1', BOTH,
                     {S5: ('cluster2', S5, 20.0, 2), S6: ('cluster2', S6, 20.0, 2)})
        xd = a.sys / 'bus/thunderbolt/devices/1-2'
        write(xd / 'rx_speed', 'garbage')
        (xd / 'tx_lanes').unlink()
        topo = a.describe()['topology']
        link = topo['cables'][1]['link']
        self.assertIsNone(link['full_speed'])
        self.assertIsNone(link['gbps'])
        self.assertFalse(any('below Gen3' in w for w in topo['warnings']))
        self.assertTrue(any('cable B' in w and 'speed unknown' in w for w in topo['warnings']))
        self.assertTrue(topo['cables'][0]['link']['full_speed'])

    def test_stream_removed_during_enumeration(self):
        a = FakeHost(self.tmp, 'cluster1', BOTH, {S5: ('cluster2', S5, 20.0, 2), S6: None})
        gone = a.configfs / '0-2.0/data1'

        class Racy(describe_mod.Describer):
            def stream_dirs(self):
                found = super().stream_dirs()
                for f in gone.iterdir():      # rmdir after listing, before reading
                    f.unlink()
                gone.rmdir()
                return found

        report = Racy(a.sys, dev_root=a.dev, hostname='cluster1', require_chardev=False).describe()
        self.assertNotIn('0-2.0/data1', report)
        self.assertIn('0-2.0/data0', report)
        self.assertTrue(any('0-2.0/data1 vanished' in w for w in report['topology']['warnings']))

    def test_vanished_service_directory_and_driver_link(self):
        a = FakeHost(self.tmp, 'cluster1', BOTH, {S5: ('cluster2', S5, 20.0, 2), S6: None})
        self.assertEqual(describe_mod.listdir(Path(self.tmp) / 'nope'), [])
        self.assertIsNone(describe_mod.driver_name(Path(self.tmp) / 'nope'))
        self.assertEqual(describe_mod.driver_name(a.sys / 'bus/thunderbolt/devices/0-2.0'),
                         'thunderbolt_stream')

    def test_journalctl_failure_is_reported(self):
        a = FakeHost(self.tmp, 'cluster1', BOTH, {S5: ('cluster2', S5, 20.0, 2), S6: None})

        class Failed:
            returncode, stdout, stderr = 1, '', 'No journal files were found.'

        original = describe_mod.subprocess.run
        describe_mod.subprocess.run = lambda *a, **k: Failed()
        try:
            out = Path(self.tmp) / 'out.json'
            with contextlib.redirect_stdout(io.StringIO()):
                describe_mod.main(['--sysfs-root', str(a.sys), '--dev-root', str(a.dev),
                                   '--hostname', 'cluster1', '--logs', '--output', str(out),
                                   '--port-labels', str(self._labels())])
        finally:
            describe_mod.subprocess.run = original
        warnings = json.loads(out.read_text())['topology']['warnings']
        self.assertIn('kernel log unavailable: journalctl failed: No journal files were found.',
                      warnings)

    def test_merge_rejects_duplicate_hostnames(self):
        a, b = pair(self.tmp, BOTH, BOTH,
                    {S5: ('cluster2', S5, 20.0, 2), S6: ('cluster2', S6, 20.0, 2)},
                    {S5: ('cluster1', S5, 20.0, 2), S6: ('cluster1', S6, 20.0, 2)})
        ra = a.describe()
        with self.assertRaisesRegex(ValueError, "duplicate host 'cluster1'"):
            describe_mod.merge([ra, ra])
        files = []
        for i, rep in enumerate((ra, ra)):
            path = Path(self.tmp) / f'r{i}.json'
            path.write_text(json.dumps(rep))
            files.append(str(path))
        with contextlib.redirect_stderr(io.StringIO()) as err, self.assertRaises(SystemExit) as ctx:
            describe_mod.main(['--merge', *files])
        self.assertEqual(ctx.exception.code, 2)
        self.assertIn('duplicate host', err.getvalue())
        self.assertEqual(len(describe_mod.merge([ra, b.describe()])['links']), 2)

    def _labels(self):
        path = Path(self.tmp) / 'ports.json'
        path.write_text(json.dumps(LABELS))
        return path


if __name__ == '__main__':
    unittest.main(verbosity=2)
