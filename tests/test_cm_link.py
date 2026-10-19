# SPDX-License-Identifier: GPL-2.0-only
"""Link handling of the patched connection manager (audit E1-E3).

Runs the actual tb_xdomain_bond_lanes_uuid_low() and
tb_xdomain_maybe_retrain_gen3() of xdomain.c and tb_start_reset_host() of
tb.c against scripted register, sideband and reset models, under
AddressSanitizer and UndefinedBehaviorSanitizer, and checks where the XDomain
state machine and tb_start() use them.
"""
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = Path(os.environ.get('USB4_SOURCE_TREE', ROOT / 'audited-overlay'))
CASES = ('bonding_retried_and_bounded', 'host_reset_result_checked', 'gen3_retrain_policy')
XDOMAIN_DEFINES = ('XDOMAIN_BONDING_RETRIES', 'XDOMAIN_GEN3_RETRAIN_MAX', 'USB4_SB_LINK_CONF_GEN3_SUPPORT',
                   'LANE_ADP_CS_0_SUPPORTED_SPEED_GEN3')


def extract(text, name):
    pattern = r'^[a-z][^;{}\n]*\b' + name + r'\([^;{}]*?\)\n\{\n.*?^\}\n'
    match = re.search(pattern, text, re.MULTILINE | re.DOTALL)
    if not match:
        raise RuntimeError(f'{name} not found')
    return match.group(0)


def define(text, name):
    match = re.search(r'^#define\s+' + name + r'\s+.+$', text, re.MULTILINE)
    if not match:
        raise RuntimeError(f'{name} not defined')
    return match.group(0) + '\n'


class CmLinkTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix='cm-link-')
        cls.addClassCleanup(cls.tmp.cleanup)
        tmp = Path(cls.tmp.name)
        cls.source = SOURCE / 'drivers/thunderbolt'
        cls.xdomain_c = (cls.source / 'xdomain.c').read_text()
        cls.tb_c = (cls.source / 'tb.c').read_text()
        parts = ['#include "cm_link_harness.h"\n']
        parts += [define(cls.xdomain_c, name) for name in XDOMAIN_DEFINES]
        parts += [extract(cls.xdomain_c, 'tb_xdomain_bond_lanes_uuid_low'),
                  extract(cls.xdomain_c, '__tb_xdomain_maybe_retrain_gen3'),
                  extract(cls.xdomain_c, 'tb_xdomain_maybe_retrain_gen3'),
                  extract(cls.tb_c, 'tb_start_reset_host'),
                  '#include "cm_link_cases.h"\n']
        unit = tmp / 'test.c'
        unit.write_text(''.join(parts))
        cls.binary = tmp / 'test'
        compiler = os.environ.get('CC', 'cc')
        if not shutil.which(compiler):
            raise RuntimeError(f'Required C compiler unavailable: {compiler}')
        result = subprocess.run([compiler, '-std=gnu11', '-g', '-O1', '-Wall', '-Wextra', '-Werror',
                                 '-Wno-unused-function', '-fsanitize=address,undefined', '-pthread',
                                 '-I', str(ROOT / 'tests'), str(unit), '-o', str(cls.binary)],
                                capture_output=True, text=True)
        if result.returncode:
            raise RuntimeError(result.stdout + result.stderr)

    def run_case(self, name):
        result = subprocess.run([str(self.binary), name], capture_output=True, text=True, timeout=20,
                                env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=1:abort_on_error=1'})
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn(f'PASS {name}', result.stdout)

    def test_state_machine_wiring(self):
        work = extract(self.xdomain_c, 'tb_xdomain_state_work')
        self.assertIn('\tcase XDOMAIN_STATE_BONDING_UUID_LOW:\n'
                      '\t\tret = tb_xdomain_bond_lanes_uuid_low(xd);\n'
                      '\t\tif (ret == -EAGAIN)\n\t\t\tgoto retry_state;\n'
                      '\t\tif (ret)\n\t\t\ttb_xdomain_bonding_failed(xd, ret);\n'
                      '\t\ttb_xdomain_queue_properties(xd);\n', work)
        self.assertIn('xd->state_retries = XDOMAIN_BONDING_RETRIES;',
                      extract(self.xdomain_c, 'tb_xdomain_queue_bonding_uuid_low'))
        self.assertRegex(work, r'\t\t\tif \(ret == -ECONNRESET\) \{\n\t\t\t\txd->state = XDOMAIN_STATE_INIT;\n'
                               r'\t\t\t\tgoto retry_state;\n\t\t\t\}\n\t\t\ttb_xdomain_failed\(xd\);')
        self.assertIn('\tif (!xd->remote_properties) {\n\t\tret = tb_xdomain_maybe_retrain_gen3(xd);\n'
                      '\t\tif (ret)\n\t\t\treturn ret;\n\t}\n',
                      extract(self.xdomain_c, 'tb_xdomain_get_properties'))

    def test_start_wiring(self):
        start = extract(self.tb_c, 'tb_start')
        self.assertIn('\tdiscover = tb_start_reset_host(tb, reset);\n\tif (discover) {', start)
        self.assertNotIn('tb_switch_reset(', start)


for _case in CASES:
    setattr(CmLinkTests, f'test_{_case}', lambda self, case=_case: self.run_case(case))
