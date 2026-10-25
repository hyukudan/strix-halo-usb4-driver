# SPDX-License-Identifier: GPL-2.0-only
"""usb4_linkstat against fake sysfs/proc trees: link rates, layout, interrupts, journal cursor handling."""
import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path, PurePosixPath
import tempfile
import unittest
from unittest import mock

REPO = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('usb4_linkstat', REPO / 'tools/usb4_linkstat.py')
linkstat = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(linkstat)
BOOT = 'fedcba9876543210fedcba9876543210'


def write(root, logical, text):
    path = Path(root).joinpath(*PurePosixPath(logical).parts[1:])
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text + '\n')


class Child:
    def __init__(self, lines, code=0):
        self.stdout = self
        self.code = code
        self.data = b''.join((l if isinstance(l, bytes) else json.dumps(l).encode()) + b'\n' for l in lines)

    def read(self, size):
        chunk, self.data = self.data[:size], self.data[size:]
        return chunk

    def close(self):
        pass

    def wait(self, timeout=None):
        return self.code

    def kill(self):
        pass


def cursor(i, boot=BOOT):
    return 's=%s;i=%x;b=%s;m=1;t=1;x=0' % ('a' * 32, i, boot)


def entry(i, message, **extra):
    return {'__CURSOR': cursor(i), '__REALTIME_TIMESTAMP': str(1000000 * i), 'MESSAGE': message, **extra}


class LinkstatTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = self.tmp.name
        write(self.root, '/proc/sys/kernel/random/boot_id', 'fedcba98-7654-3210-fedc-ba9876543210')
        for router, speed in (('0-2', '20.0 Gb/s'), ('1-2', '10.0 Gb/s')):
            for direction in ('rx', 'tx'):
                write(self.root, f'/sys/bus/thunderbolt/devices/{router}/{direction}_speed', speed)
                write(self.root, f'/sys/bus/thunderbolt/devices/{router}/{direction}_lanes', '2')
        write(self.root, '/sys/bus/thunderbolt/devices/0-0/unique_id', 'host-router')   # no link of its own
        write(self.root, '/sys/bus/thunderbolt/devices/1-2/rx_lanes', 'x')             # unreadable lanes
        write(self.root, '/proc/interrupts', '      CPU0\n'
              ' 53:   7  IR-PCI-MSIX-0000:c7:00.5    0-edge      thunderbolt\n'
              ' 56:   9  IR-PCI-MSIX-0000:c7:00.5    3-edge      thunderbolt\n'
              ' 60:   4  IR-PCI-MSIX-0000:01:00.0    0-edge      nvme0q0\n')
        self.entries = [entry(1, 'Linux version 7.0', _SOURCE_MONOTONIC_TIMESTAMP='0'),
                        entry(2, 'tbstream: GPU context still quarantined after rmdir'),
                        entry(3, 'thunderbolt 0-2: new host found')]

    def collect(self, payload, *children):
        calls = []
        queue = list(children)

        def runner(argv, **kwargs):
            calls.append(argv)
            return queue.pop(0)
        report = linkstat.Collector(linkstat.Reader(self.root, runner=runner)).collect(payload)
        return report, calls

    def test_links_flags_and_interrupts(self):
        report, calls = self.collect({})
        self.assertEqual(calls, [])                                         # no journal unless asked
        links = {l['router']: l for l in report['links']}
        self.assertEqual(sorted(links), ['0-2', '1-2'])
        self.assertTrue(links['0-2']['full_rate'])
        self.assertIsNone(links['1-2']['rx']['gbps'])                       # unknown, never inferred
        self.assertEqual(links['1-2']['below_full_rate'], ['tx'])
        self.assertFalse(links['1-2']['full_rate'])
        self.assertEqual([(v['vector'], v['ring'], v['count']) for v in report['interrupts']['vectors']],
                         [(0, 'control', 7), (3, 'data', 9)])
        self.assertEqual(report['layout']['status'], 'unavailable')
        self.assertEqual(report['journal']['status'], 'skipped')

    def test_journal_initial_follow_and_lost_cursor(self):
        report, calls = self.collect({'journal': {'boot_id': None, 'cursor': None}}, Child(self.entries))
        journal = report['journal']
        self.assertEqual((journal['mode'], journal['complete'], journal['counts']['quarantine']), ('initial', True, 1))
        self.assertIn('--boot=' + BOOT, calls[0])
        report, calls = self.collect({'journal': {'boot_id': BOOT, 'cursor': cursor(2)}}, Child(self.entries[1:]))
        self.assertEqual((report['journal']['mode'], report['journal']['entries']), ('follow', 1))
        self.assertIn('--cursor=' + cursor(2), calls[0])
        report, _ = self.collect({'journal': {'boot_id': BOOT, 'cursor': cursor(2)}},
                                 Child(self.entries[2:]), Child(self.entries))
        self.assertEqual(report['journal']['mode'], 'cursor_lost')
        report, _ = self.collect({'journal': {'boot_id': None, 'cursor': None}}, Child(self.entries[1:]))
        self.assertFalse(report['journal']['complete'])                    # the boot's first message is gone

    def test_cli_prints_json_and_self_check_passes(self):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            self.assertEqual(linkstat.main(['--root', self.root]), 0)
        self.assertEqual(json.loads(out.getvalue())['schema'], 'usb4-linkstat-v1')
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(linkstat.main(['--self-check']), 0)
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            linkstat.main(['--cursor', 'bad cursor\n'])

    def test_printed_output_never_exceeds_the_cap(self):
        big = {'schema': 'usb4-linkstat-v1', 'links': [],
               'journal': {'recent': [{'ts': 1.0, 'kind': 'host_reset', 'message': 'x' * 200}] * 2000}}
        with mock.patch.object(linkstat.Collector, 'collect', lambda self, payload: linkstat.cap(big)):
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                linkstat.main(['--root', self.root])
        self.assertLessEqual(len(out.getvalue()), linkstat.MAX_OUTPUT)
        self.assertTrue(json.loads(out.getvalue())['truncated'])

    def test_stream_devices_and_configfs_are_never_opened(self):
        write(self.root, '/run/usb4stream-layout.json', json.dumps({
            '0-2.0/data0': {'device': '/sys/kernel/config/x', 'index': 0, 'config': {'gpu_timing': 'none'}},
            'version': 2, 'topology': {}}))
        real = os.open

        def guarded(path, *args, **kwargs):
            text = str(path).replace('\\', '/')
            if '/dev/' in text or '/sys/kernel/config' in text:
                raise AssertionError('opened ' + text)
            return real(path, *args, **kwargs)
        with mock.patch.object(os, 'open', guarded):
            report, _ = self.collect({})
        stream = report['layout']['streams'][0]
        self.assertEqual(stream['device'], '/dev/tbstream0')               # built from the index, not the layout text
        self.assertEqual(stream['config'], {})


if __name__ == '__main__':
    unittest.main()
