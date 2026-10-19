#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Lab kernel identity, service condition, USB4NET policy and ring budget agree."""
import configparser
import fnmatch
import importlib.util
import os
from pathlib import Path
import re
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[1]
LAB = REPO / 'tools/lab'


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


ck = load('check_kconfig', REPO / 'tools/check_kconfig.py')
diag = load('usb4_diagnose', REPO / 'tools/usb4_diagnose.py')

LAB_BUILD = '7.3.0-rc5' + ck.LAB_PREFIX + '20261020a'
OTHERS = ('7.3.0-rc5', '6.17.0-5-generic', '7.3.0-rc5-other', '7.3.0-rc6-strix-gpu-bosgame-x')


def unit(path):
    parser = configparser.ConfigParser(strict=False, interpolation=None,
                                       comment_prefixes=('#',), inline_comment_prefixes=None)
    parser.optionxform = str
    parser.read_string(path.read_text())
    return parser


def conditions(path):
    """Effective ConditionKernelVersion values (an empty assignment resets)."""
    values = []
    for line in path.read_text().splitlines():
        if line.startswith('ConditionKernelVersion='):
            value = line.split('=', 1)[1]
            values = [] if not value else values + [value]
    return values


def allowed(values, release):
    return all(fnmatch.fnmatchcase(release, v) for v in values)


class LabPolicyTests(unittest.TestCase):
    def test_service_accepts_every_lab_build_and_no_other_kernel(self):
        for path in (LAB / 'usb4stream-lab.service', LAB / 'usb4stream-describe-refresh.service',
                     LAB / 'usb4stream-lab.service.d/10-reviewed-kernel.conf.example'):
            with self.subTest(path=path.name):
                values = conditions(path)
                self.assertTrue(values)
                self.assertTrue(allowed(values, LAB_BUILD))
                self.assertTrue(allowed(values, '7.3.0-rc5-strix-gpu-bosgame-20261019det'))
                for release in OTHERS:
                    self.assertFalse(allowed(values, release), release)

    def test_service_is_installable(self):
        service = unit(LAB / 'usb4stream-lab.service')
        self.assertEqual(service['Install']['WantedBy'], 'multi-user.target')
        self.assertEqual(service['Service']['ExecStart'], '/usr/local/sbin/setup-usb4stream-lab')

    def test_modprobe_policy_blocks_usb4net_only_on_lab_kernels(self):
        text = (LAB / 'usb4stream-dedicated.conf').read_text()
        match = re.search(r'^install thunderbolt_net /bin/sh -c \'case "\$\(uname -r\)" in '
                          r'(\S+)\) exit 0;; \*\) exec /sbin/modprobe --ignore-install '
                          r'thunderbolt_net;; esac\'$', text, re.M)
        self.assertIsNotNone(match, text)
        pattern = match[1]
        self.assertTrue(fnmatch.fnmatchcase(LAB_BUILD, pattern))
        for release in OTHERS:
            self.assertFalse(fnmatch.fnmatchcase(release, pattern), release)
        self.assertEqual(pattern, conditions(LAB / 'usb4stream-lab.service')[0])
        self.assertEqual(pattern, '7.3.0-rc5' + ck.LAB_PREFIX + '*')

    def test_udev_rule_only_starts_the_read_only_refresh(self):
        rule = (LAB / '90-usb4stream-describe.rules').read_text()
        self.assertIn('start usb4stream-describe-refresh.service', rule)
        refresh = unit(LAB / 'usb4stream-describe-refresh.service')
        self.assertIn('usb4stream-describe', refresh['Service']['ExecStart'])
        self.assertNotIn('setup-usb4stream-lab', refresh['Service']['ExecStart'])

    def test_install_doc_enables_service_and_keeps_previous_kernel(self):
        doc = (REPO / 'docs/INSTALL-AND-TEST.md').read_text()
        for needle in ('systemctl enable usb4stream-lab.service',
                       'systemctl is-enabled usb4stream-lab.service',
                       'install -m 0644 tools/lab/usb4stream-dedicated.conf /etc/modprobe.d/',
                       'tools/check_kconfig.py', '--kernelrelease',
                       'previous kernel', 'usb4_diagnose.py --require-budget'):
            self.assertIn(needle, doc)


class RingBudgetTests(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.root = Path(self._tmp.name)
        self.sys = self.root / 'sys'
        self.modprobe = self.root / 'modprobe.d'
        self.modprobe.mkdir()
        devices = self.sys / 'bus/thunderbolt/devices'
        drivers = self.sys / 'bus/thunderbolt/drivers'
        for name in ('thunderbolt_stream', 'thunderbolt-net'):
            (drivers / name).mkdir(parents=True)
        for domain in (0, 1):
            svc = devices / f'{domain}-2.0'
            svc.mkdir(parents=True)
            os.symlink(drivers / 'thunderbolt_stream', svc / 'driver')
            for stream in ('data0', 'data1'):
                (self.sys / f'kernel/config/thunderbolt/stream/{domain}-2.0/{stream}').mkdir(parents=True)
        (devices / '0-0:2.1').mkdir()          # router port device, not a service
        for domain, pci in ((0, '0000:c7:00.5'), (1, '0000:c7:00.6')):
            (self.sys / f'bus/pci/devices/{pci}/domain{domain}').mkdir(parents=True)
        self.interrupts = self.root / 'interrupts'
        self.held(0, 0)

    def held(self, rings0, rings1):
        """/proc/interrupts with control (2) + data ring vectors per NHI."""
        lines = ['           CPU0       CPU1']
        irq = 50
        for pci, data in (('0000:c7:00.5', rings0), ('0000:c7:00.6', rings1)):
            for vector in range(2 + data):
                lines.append(f' {irq}:          0          9  IR-PCI-MSIX-{pci}    '
                             f'{vector}-edge      thunderbolt')
                irq += 1
        lines.append(' 99:          0          0  IR-PCI-MSIX-0000:c4:00.0    0-edge      nvme0q0')
        self.interrupts.write_text('\n'.join(lines) + '\n')

    def budget(self, kernel=LAB_BUILD, **kw):
        return diag.ring_budget(self.sys, modprobe_dir=self.modprobe, kernel=kernel,
                                interrupts=self.interrupts, **kw)

    def test_lab_layout_fits_and_requires_policy(self):
        report = self.budget()
        self.assertEqual({d: v['planned'] for d, v in report['domains'].items()}, {'0': 2, '1': 2})
        self.assertEqual({d: v['held_data_rings'] for d, v in report['domains'].items()},
                         {'0': 0, '1': 0})
        self.assertEqual(len(report['problems']), 1)
        self.assertIn('usb4stream-dedicated.conf missing', report['problems'][0])
        (self.modprobe / 'usb4stream-dedicated.conf').write_text('x')
        self.assertEqual(self.budget()['problems'], [])
        self.assertEqual(self.budget('6.17.0-5-generic')['problems'], [])

    def test_usb4net_consumer_is_reported(self):
        (self.modprobe / 'usb4stream-dedicated.conf').write_text('x')
        net = self.sys / 'bus/thunderbolt/devices/1-2.1'
        net.mkdir()
        os.symlink(self.sys / 'bus/thunderbolt/drivers/thunderbolt-net', net / 'driver')
        (self.sys / 'module/thunderbolt_net').mkdir(parents=True)
        report = self.budget()
        self.assertEqual(report['domains']['1']['usb4net_services'], ['1-2.1'])
        self.assertEqual(report['domains']['1']['planned'], 3)
        self.assertTrue(report['thunderbolt_net_loaded'])
        self.assertEqual(len(report['problems']), 3)
        self.assertTrue(any('budget of 2' in p for p in report['problems']))

    def test_held_rings_come_from_msix_vectors(self):
        (self.modprobe / 'usb4stream-dedicated.conf').write_text('x')
        self.assertEqual(diag.held_rings(self.interrupts.read_text()),
                         {'0000:c7:00.5': 0, '0000:c7:00.6': 0})
        self.held(4, 2)             # both streams open on NHI0, one on NHI1
        report = self.budget()
        self.assertEqual(report['domains']['0']['held_pairs'], 2)
        self.assertEqual(report['domains']['1']['held_data_rings'], 2)
        self.assertEqual(len(report['problems']), 2)
        self.assertTrue(all('already held' in p for p in report['problems']))
        self.assertEqual(self.budget(allow_held=True)['problems'], [])

    def test_orphaned_gpu_context_after_rmdir_is_detected(self):
        # rmdir removed both groups of domain 1, but a retained GPU context
        # still holds a TX/RX pair: configfs shows nothing, the vectors do.
        (self.modprobe / 'usb4stream-dedicated.conf').write_text('x')
        for stream in ('data0', 'data1'):
            (self.sys / f'kernel/config/thunderbolt/stream/1-2.0/{stream}').rmdir()
        self.held(0, 2)
        report = self.budget(allow_held=True)
        self.assertEqual(report['domains']['1']['planned'], 0)
        self.assertEqual(report['domains']['1']['held_pairs'], 1)
        self.assertEqual(len(report['problems']), 1)
        self.assertIn('2 data rings held but only 0 configured consumers', report['problems'][0])
        self.assertIn('gpu_reap_orphans', report['problems'][0])

    def test_quarantine_and_kernel_log_evidence(self):
        (self.modprobe / 'usb4stream-dedicated.conf').write_text('x')
        (self.sys / 'kernel/config/thunderbolt/stream/0-2.0/data1/gpu_quarantine').write_text('1\n')
        (self.sys / 'kernel/config/thunderbolt/stream/1-2.0/data0/busy_poll').write_text('1\n')
        log = ['oct 20 kernel: tbstream: tbstream3: GPU context still quarantined after rmdir, '
               'retry with /sys/module/thunderbolt_stream/parameters/gpu_reap_orphans',
               'oct 20 kernel: thunderbolt 0000:c7:00.6: ring 3 leaked by an abandoned DMA owner',
               'oct 20 kernel: thunderbolt domain0: host interface reset after DMA teardown '
               'pending: NHI in use (2 TX and 2 RX rings allocated besides control; ...)']
        report = self.budget(kernel_log=log)
        problems = report['problems']
        self.assertEqual(len(problems), 3, problems)
        self.assertIn('0-2.0/data1 has a quarantined GPU context', problems[0])
        self.assertTrue(any('orphaned GPU context kept after rmdir' in p for p in problems))
        self.assertTrue(any('ring leaked by an abandoned DMA owner' in p for p in problems))
        self.assertTrue(any('busy_poll streams (1-2.0/data0)' in n for n in report['notes']))
        self.assertTrue(any('teardown reset pending' in n for n in report['notes']))

    def test_unreadable_interrupts_is_reported(self):
        self.interrupts.unlink()
        report = self.budget()
        self.assertIsNone(report['domains']['0']['held_data_rings'])
        self.assertTrue(any('held rings unknown' in n for n in report['notes']))

    def test_require_budget_exit_status(self):
        import contextlib
        import io
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            status = diag.main(['--sysfs-root', str(self.sys), '--modprobe-dir', str(self.modprobe),
                                '--interrupts', str(self.interrupts), '--require-budget'])
        expected = 1 if diag.platform.release().startswith('7.3.0-rc5' + ck.LAB_PREFIX) else 0
        self.assertEqual(status, expected)
        (self.sys / 'kernel/config/thunderbolt/stream/0-2.0/data2').mkdir()
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(diag.main(['--sysfs-root', str(self.sys), '--modprobe-dir',
                                        str(self.modprobe), '--interrupts', str(self.interrupts),
                                        '--require-budget']), 1)
        self.held(2, 0)
        (self.sys / 'kernel/config/thunderbolt/stream/0-2.0/data2').rmdir()
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(diag.main(['--sysfs-root', str(self.sys), '--modprobe-dir',
                                        str(self.modprobe), '--interrupts', str(self.interrupts),
                                        '--require-budget', '--allow-held']), expected)


if __name__ == '__main__':
    unittest.main(verbosity=2)
