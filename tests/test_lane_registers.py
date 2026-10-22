#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""usb4_diagnose.py --registers: LANE_ADP_CS_0/1, sideband LINK_CONF/TXFFE vs sysfs.

Fixtures reproduce the node A capture of 2026-10-19 21:44 (mixed state: one
cable Gen3 x2, one Gen2 x2) in the debugfs file formats.
"""
import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('usb4_diagnose', REPO / 'tools/usb4_diagnose.py')
diag = importlib.util.module_from_spec(spec)
spec.loader.exec_module(diag)

# domain -> (sysfs Gb/s per lane, lane0 CS0, lane1 CS0, CS1, LINK_CONF, TXFFE), from the capture.
CAPTURE = {
    0: ('20.0', 0x003c01c0, 0x003c0100, 0x4824003c, '0x03 0xf3 0x03', '0x7f 0x7f 0x80 0x80'),
    1: ('10.0', 0x003c01c0, 0x003c0100, 0x4828003c, '0x03 0xf3 0x03', '0x70 0x70 0x80 0x80'),
}
EXPECTED_LINES = [
    '  0-0 port2 CS0=0x003c01c0 CS1=0x4824003c target_speed=0xc target_width=3 cur_speed=0x4 '
    'cur_width=2 lane_dis=0 lane_bond=0',
    '  0-0 port3 CS0=0x003c0100 CS1=0x4824003c target_speed=0xc target_width=3 cur_speed=0x4 '
    'cur_width=2 lane_dis=0 lane_bond=0',
    '  LINK_CONF 0x0c 0x03 0xf3 0x03  TXFFE 0x0d 0x7f 0x7f 0x80 0x80',
    '  1-0 port2 CS0=0x003c01c0 CS1=0x4828003c target_speed=0xc target_width=3 cur_speed=0x8 '
    'cur_width=2 lane_dis=0 lane_bond=0',
    '  1-0 port3 CS0=0x003c0100 CS1=0x4828003c target_speed=0xc target_width=3 cur_speed=0x8 '
    'cur_width=2 lane_dis=0 lane_bond=0',
    '  LINK_CONF 0x0c 0x03 0xf3 0x03  TXFFE 0x0d 0x70 0x70 0x80 0x80',
]


def write(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)


def regs(cs0, cs1):
    rows = ['# offset relative_offset cap_id vs_cap_id value',
            '0x0000    0 0x00 0x00 0x00000000',
            '0x0001    1 0x00 0x00 0x00080300',
            f'0x0080    0 0x01 0x00 0x{cs0:08x}',
            f'0x0081    1 0x01 0x00 0x{cs1:08x}',
            '0x00d2   18 0x06 0x00 0x00003510']
    return '\n'.join(rows) + '\n'


def sb_regs(link_conf, txffe):
    return ('# register value\n0x00 0x38 0x04 0x00 0x00\n0x05 <not accessible>\n'
            f'0x08 0x00 0x00 0x00 0x00\n0x0c {link_conf}\n0x0d {txffe}\n0x0e <not accessible>\n')


class LaneRegisterTests(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        root = Path(self._tmp.name)
        self.sys, self.debugfs = root / 'sys', root / 'debug'
        devices = self.sys / 'bus/thunderbolt/devices'
        for domain, (speed, cs0a, cs0b, cs1, link_conf, txffe) in CAPTURE.items():
            (devices / f'{domain}-0/usb4_port2').mkdir(parents=True)
            xd = devices / f'{domain}-2'
            for d in ('rx', 'tx'):
                write(xd / f'{d}_speed', f'{speed} Gb/s\n')
                write(xd / f'{d}_lanes', '2\n')
            base = self.debugfs / f'{domain}-0'
            write(base / 'port2/regs', regs(cs0a, cs1))
            write(base / 'port3/regs', regs(cs0b, cs1))
            write(base / 'port2/sb_regs', sb_regs(link_conf, txffe))

    def lanes(self):
        return diag.lane_registers(self.sys, self.debugfs)

    def test_decode_matches_tb_regs_fields(self):
        lane = diag.decode_lane(0x003c01c0, 0x4828003c)
        self.assertEqual((lane['supported_speed'], lane['supported_width']), ('0xc', 3))
        self.assertEqual((lane['target_speed'], lane['target_width']), ('0xc', 3))
        self.assertEqual((lane['cur_speed'], lane['cur_generation'], lane['cur_gbps_per_lane'],
                          lane['cur_width']), ('0x8', 'Gen2', 10.0, 2))
        self.assertFalse(lane['lane_disabled'] or lane['lane_bonded'])
        self.assertEqual(diag.decode_lane(0, 0x4824003c)['cur_generation'], 'Gen3')
        self.assertTrue(diag.decode_lane(0, 1 << 14 | 1 << 15)['lane_disabled'])

    def test_capture_agrees_with_sysfs_and_flags_gen2(self):
        lanes = self.lanes()
        self.assertEqual(lanes['mismatches'], [])
        self.assertEqual(lanes['errors'], [])
        self.assertEqual([(p['router'], sorted(p['adapters'])) for p in lanes['ports']],
                         [('0-0', ['2', '3']), ('1-0', ['2', '3'])])
        self.assertEqual(lanes['warnings'],
                         ['1-0 port2: trained Gen2 although the target speed is Gen3 '
                          '(CS1 0x4828003c)'])
        sb = lanes['ports'][0]['sideband']
        self.assertEqual(sb['link_conf'], ['0x03', '0xf3', '0x03'])
        self.assertEqual(sb['txffe'], ['0x7f', '0x7f', '0x80', '0x80'])
        self.assertTrue(sb['gen3_support_lab_decode'])

    def test_lanecap_text_reproduces_the_lab_capture(self):
        report = dict(kernel='7.3.0-rc5-strix-gpu-bosgame-20261019det', lanes=self.lanes())
        text = diag.lanecap_text(report, 'node-a').splitlines()
        self.assertEqual(text[0], '### node-a 7.3.0-rc5-strix-gpu-bosgame-20261019det')
        self.assertEqual([l for l in text if l.startswith('  ') and not l.startswith('  !')],
                         EXPECTED_LINES)
        self.assertIn('domain1 port2 sysfs rx=10.0 Gb/s x2 tx=10.0 Gb/s x2 peer=1-2', text)

    def test_sysfs_register_mismatch(self):
        write(self.sys / 'bus/thunderbolt/devices/1-2/rx_speed', '20.0 Gb/s\n')
        write(self.sys / 'bus/thunderbolt/devices/1-2/tx_lanes', '1\n')
        mismatches = self.lanes()['mismatches']
        self.assertEqual(len(mismatches), 2, mismatches)
        self.assertIn('sysfs rx_speed 20 Gb/s but LANE_ADP_CS_1 current speed 0x8 (Gen2)',
                      mismatches[0])
        self.assertIn('sysfs tx_lanes 1 but LANE_ADP_CS_1 current width 2', mismatches[1])

    def test_no_gen3_advertised_and_lane_disagreement(self):
        write(self.debugfs / '1-0/port2/sb_regs', sb_regs('0x03 0xd3 0x03', '0x70 0x70 0x80 0x80'))
        write(self.debugfs / '1-0/port3/regs', regs(0x003c0100, 0x4818003c))
        warnings = self.lanes()['warnings']
        self.assertIn('1-0 port2: sideband LINK_CONF advertises no Gen3 support (lab decode)',
                      warnings)
        self.assertIn('1-0 port2: the two lane adapters disagree on current speed/width', warnings)

    def run_main(self, *extra):
        with contextlib.redirect_stdout(io.StringIO()) as out, \
                contextlib.redirect_stderr(io.StringIO()) as err:
            status = diag.main(['--sysfs-root', str(self.sys), '--debugfs-root', str(self.debugfs),
                                '--modprobe-dir', str(self.sys), *extra])
        return status, out.getvalue(), err.getvalue()

    def test_cli_json_lanecap_and_exit_status(self):
        status, out, _ = self.run_main('--registers')
        self.assertEqual(status, 0)
        self.assertEqual(len(json.loads(out)['lanes']['ports']), 2)
        status, out, _ = self.run_main('--lanecap')
        self.assertEqual(status, 0)
        self.assertIn(EXPECTED_LINES[3], out)
        write(self.sys / 'bus/thunderbolt/devices/0-2/rx_speed', '10.0 Gb/s\n')
        status, _, err = self.run_main('--registers')
        self.assertEqual(status, 1)
        self.assertIn('sysfs/register mismatch', err)
        status, out, _ = self.run_main()
        self.assertNotIn('lanes', json.loads(out))

    def test_unreadable_debugfs_is_reported_not_hidden(self):
        with contextlib.redirect_stdout(io.StringIO()) as out, \
                contextlib.redirect_stderr(io.StringIO()) as err:
            status = diag.main(['--sysfs-root', str(self.sys), '--debugfs-root',
                                str(Path(self._tmp.name) / 'missing'), '--registers'])
        self.assertEqual(status, 1)
        lanes = json.loads(out.getvalue())['lanes']
        self.assertTrue(lanes['errors'])
        self.assertTrue(all(not any(p['adapters'].values()) for p in lanes['ports']))
        self.assertIn('lane registers:', err.getvalue())


if __name__ == '__main__':
    unittest.main(verbosity=2)
