# SPDX-License-Identifier: GPL-2.0-only
"""Domain and NHI teardown of the patched core.

Runs the actual tb_domain_remove() (domain.c) with the CM's
tb_remove_xdomains() (tb.c): XDomains are unregistered, and their service
drivers stop their rings, before tb_stop() deactivates the DMA paths and
removes the routers; nothing is unregistered twice against the hotplug work.
Runs the actual nhi_reset() of nhi.c: the quirked v1 AMD host interfaces are
reset at probe. Checks that a failed probe waits for its domain with the
bounded tb_domain_wait_released(). Routers, services, MMIO and the
workqueue are logged models, not hardware.
"""
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent
DOMAIN_CASES = ('xdomains_unregistered_before_stop', 'racing_hotplug_work', 'cm_without_hook')
NHI_CASES = ('v1_quirked_interface_reset', 'v1_untouched', 'v2_router_reset')
NHI_DEFINES = ('REG_CAPS', 'REG_CAPS_VERSION_MASK', 'REG_CAPS_VERSION_2', 'REG_HOST_INTERFACE_RESET',
               'REG_HOST_INTERFACE_RESET_RST', 'REG_RESET', 'REG_RESET_HRR')
NHI_PRELUDE = '''#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint32_t u32;
typedef int64_t ktime_t;
#define BIT(n) (1U << (n))
#define GENMASK(h, l) (((~0U) << (l)) & (~0U >> (31 - (h))))
#define FIELD_GET(mask, v) (((v) & (mask)) >> __builtin_ctz(mask))
struct device { int unused; };
struct tb_nhi { void *iobase; struct device *dev; u32 quirks; };
static bool host_reset = true;
'''


def extract(text, name):
    pattern = r'^[a-z][^;{}\n]*\b' + name + r'\([^;{}]*?\)\n\{\n.*?^\}\n'
    match = re.search(pattern, text, re.MULTILINE | re.DOTALL)
    if not match:
        raise RuntimeError(f'{name} not found')
    return match.group(0)


def extract_struct(text, name):
    match = re.search(r'^struct ' + name + r' \{\n.*?^\};\n', text, re.MULTILINE | re.DOTALL)
    if not match:
        raise RuntimeError(f'struct {name} not found')
    return match.group(0)


def define(text, name):
    match = re.search(r'^#define\s+' + name + r'\s+(.+)$', text, re.MULTILINE)
    if not match:
        raise RuntimeError(f'{name} not defined')
    return f'#define {name} {match.group(1).strip()}\n'


def build(tmp, name, source):
    unit = tmp / f'{name}.c'
    unit.write_text(source)
    compiler = os.environ.get('CC', 'cc')
    if not shutil.which(compiler):
        raise RuntimeError(f'Required C compiler unavailable: {compiler}')
    binary = tmp / name
    result = subprocess.run([compiler, '-std=gnu11', '-g', '-O1', '-Wall', '-Wextra', '-Werror',
                             '-Wno-unused-function', '-fno-omit-frame-pointer', '-fno-pie', '-no-pie',
                             '-fsanitize=address,undefined', '-I', str(ROOT), str(unit), '-o', str(binary)],
                            capture_output=True, text=True)
    if result.returncode:
        raise RuntimeError(result.stdout + result.stderr)
    return binary


class DomainTeardownTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if 'USB4_SOURCE_TREE' not in os.environ:
            raise unittest.SkipTest('USB4_SOURCE_TREE with the patched sources is required')
        cls.source = Path(os.environ['USB4_SOURCE_TREE']) / 'drivers/thunderbolt'
        if not (cls.source / 'tb.c').exists():
            raise unittest.SkipTest('tb.c is not part of this source tree')
        cls.tmp = tempfile.TemporaryDirectory(prefix='domain-teardown-')
        cls.addClassCleanup(cls.tmp.cleanup)
        tmp = Path(cls.tmp.name)
        tb_c = (cls.source / 'tb.c').read_text()
        domain_c = (cls.source / 'domain.c').read_text()
        cls.domain = build(tmp, 'domain', '\n'.join([
            '#include "domain_teardown_harness.h"\n',
            extract(tb_c, 'tb_remove_switch_xdomains'),
            extract(tb_c, 'tb_remove_xdomains'),
            extract_struct(domain_c, 'tb_xdomain_unregister_work'),
            extract(domain_c, 'tb_domain_unregister_xdomains_work'),
            extract(domain_c, 'tb_domain_unregister_xdomains_sync'),
            extract(domain_c, 'tb_domain_remove'),
            '#include "domain_teardown_cases.h"\n']))
        nhi_c = (cls.source / 'nhi.c').read_text()
        regs = (cls.source / 'nhi_regs.h').read_text()
        quirks = (cls.source / 'nhi.h').read_text()
        xdomain_c = (cls.source / 'xdomain.c').read_text()
        cls.presence = build(tmp, 'presence', '#include "xdomain_presence_cases.h"\n' +
                             extract(xdomain_c, 'tb_xdomain_presence'))
        # The MMIO model and the cases come first; the functions follow.
        cls.nhi = build(tmp, 'nhi', ''.join(
            [NHI_PRELUDE] + [define(regs, name) for name in NHI_DEFINES] +
            [define(quirks, 'QUIRK_RESET_INTERFACE_ON_PROBE'),
             'static void nhi_reset(struct tb_nhi *nhi);\n', '#include "nhi_reset_cases.h"\n',
             extract(nhi_c, 'nhi_reset_interface'), extract(nhi_c, 'nhi_reset')]))

    def run_case(self, binary, case):
        result = subprocess.run([str(binary), case], capture_output=True, text=True, timeout=30,
                                env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=1:abort_on_error=1'})
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn(f'PASS {case}', result.stdout)

    def test_software_cm_registers_hook(self):
        ops = re.search(r'^static const struct tb_cm_ops tb_cm_ops = \{\n.*?^\};\n',
                        (self.source / 'tb.c').read_text(), re.MULTILINE | re.DOTALL)
        self.assertIsNotNone(ops)
        self.assertIn('\t.remove_xdomains = tb_remove_xdomains,\n', ops.group(0))

    def test_xdomain_presence(self):
        """0019: what open() asks before waiting for a peer."""
        self.run_case(self.presence, 'presence')
        header = (self.source.parent.parent / 'include/linux/thunderbolt.h').read_text()
        self.assertIn('int tb_xdomain_presence(unsigned int domain, u64 route);', header)
        self.assertIn('EXPORT_SYMBOL_GPL(tb_xdomain_presence);', (self.source / 'xdomain.c').read_text())

    def test_teardown_reset_wiring(self):
        """0018: where the teardown reset is scheduled and cancelled (its
        behaviour is run in test_dma_recovery)."""
        domain_c = (self.source / 'domain.c').read_text()
        tb_c = (self.source / 'tb.c').read_text()
        disconnect = extract(domain_c, 'tb_domain_disconnect_xdomain_paths')
        self.assertRegex(disconnect, r'\tif \(!ret\)\n\t\ttb_domain_schedule_teardown_reset\(tb\);\n\treturn ret;')
        hotplug = extract(tb_c, 'tb_handle_hotplug')
        self.assertRegex(hotplug, r'__tb_disconnect_xdomain_paths\(tb, xd, -1, -1, -1, -1\);\n'
                                  r'(\t+/\*[^\n]*\*/\n)?\t+tb_domain_schedule_teardown_reset\(tb\);')
        self.assertIn('INIT_DELAYED_WORK(&tb->teardown_reset_work, tb_domain_teardown_reset_work);',
                      extract(domain_c, 'tb_domain_alloc'))
        for name in ('tb_domain_release', 'tb_domain_remove', 'tb_domain_suspend', 'tb_domain_freeze_noirq'):
            self.assertIn('cancel_delayed_work_sync(&tb->teardown_reset_work);', extract(domain_c, name), name)

    def test_failed_probe_wait_is_bounded(self):
        nhi_c = (self.source / 'nhi.c').read_text()
        probe = extract(nhi_c, 'nhi_probe')
        self.assertRegex(probe, r'tb_domain_put\(tb\);\n(\t\t/\*[^\n]*\*/\n)?\t\ttb_domain_wait_released\(tb, nhi\);\n'
                                r'\t\tnhi_shutdown\(nhi\);')
        for name in ('nhi.c', 'pci.c'):
            self.assertNotIn('wait_for_completion(&nhi->domain_released)', (self.source / name).read_text())

    def test_strix_halo_nhis_are_quirked(self):
        pci_c = (self.source / 'pci.c').read_text()
        amd = re.search(r'\} else if \(pdev->vendor == PCI_VENDOR_ID_AMD\) \{\n.*?\n\t\}\n', pci_c, re.DOTALL)
        self.assertIsNotNone(amd)
        for device in ('M70H_NHI0', 'M70H_NHI1'):
            self.assertIn(f'case PCI_DEVICE_ID_AMD_1AH_{device}:', amd.group(0))
        self.assertIn('nhi->quirks |= QUIRK_RESET_INTERFACE_ON_PROBE;', amd.group(0))


for _case in DOMAIN_CASES:
    setattr(DomainTeardownTests, f'test_{_case}', lambda self, case=_case: self.run_case(self.domain, case))
for _case in NHI_CASES:
    setattr(DomainTeardownTests, f'test_nhi_{_case}', lambda self, case=_case: self.run_case(self.nhi, case))

if __name__ == '__main__':
    unittest.main()
