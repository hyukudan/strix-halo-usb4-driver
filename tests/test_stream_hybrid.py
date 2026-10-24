"""Compile actual 0049 NHI lease + stream paths under ASAN/UBSAN (CPU only)."""
import importlib.util
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('host_path', ROOT / 'test_stream_host_path.py')
host = importlib.util.module_from_spec(spec)
spec.loader.exec_module(host)


def core_function(text, name):
    m = re.search(r'^(?:int|void) ' + name + r'\([^;]*?\)\n\{\n.*?^\}\n', text, re.M | re.S)
    if not m:
        raise AssertionError(name)
    return m.group(0)


def build_hybrid_unit(tree, tmp):
    stream = (tree / 'drivers/thunderbolt/stream.c').read_text()
    nhi = (tree / 'drivers/thunderbolt/nhi.c').read_text()
    tmp = Path(tmp)
    for p in ROOT.glob('*.h'):
        shutil.copyfile(p, tmp / p.name)
    harness = (tmp / 'stream_harness.h').read_text()
    harness = harness.replace('typedef int64_t ktime_t;', '''typedef int64_t ktime_t;
    typedef pthread_mutex_t spinlock_t;
    #define spin_lock_init(p) assert(!pthread_mutex_init(p, NULL))
    #define spin_lock(p) assert(!pthread_mutex_lock(p))
    #define spin_unlock(p) assert(!pthread_mutex_unlock(p))
    #define spin_lock_irqsave(p, flags) do { (flags) = 0; spin_lock(p); } while (0)
    #define spin_unlock_irqrestore(p, flags) do { (void)(flags); spin_unlock(p); } while (0)
    #define EXPORT_SYMBOL_GPL(x)
    enum hrtimer_restart { HRTIMER_NORESTART, HRTIMER_RESTART };
    #define HRTIMER_MODE_ABS_HARD 1
    struct hrtimer { enum hrtimer_restart (*fn)(struct hrtimer *); bool armed; ktime_t expires; };
    static void hrtimer_setup(struct hrtimer *t, enum hrtimer_restart (*fn)(struct hrtimer *), int clock, int mode) { t->fn = fn; t->armed = false; }
    static void hrtimer_cancel(struct hrtimer *t) { t->armed = false; }
    static void hrtimer_start(struct hrtimer *t, ktime_t when, int mode) { t->expires = when; t->armed = true; }
    static void hrtimer_set_expires(struct hrtimer *t, ktime_t when) { t->expires = when; }
    #include <linux/strix_host_stream.h>''')
    harness += '\n#define __user\nstatic bool user_copy_fault;\nstatic int copy_from_user(void *to, const void *from, size_t n) { if (user_copy_fault) return 1; memcpy(to, from, n); return 0; }\n'
    harness += '\nstatic int kstrtobool(const char *s, bool *value) { if (!strcmp(s, "1")) { *value=true; return 0; } if (!strcmp(s,"0")) { *value=false; return 0; } return -EINVAL; }\n'
    harness = harness.replace('struct tb_nhi { struct device dev;', 'struct tb_nhi { spinlock_t lock; bool going_away; struct device dev;')
    harness = harness.replace('bool is_tx, running, masked;', 'bool is_tx, running, masked, poll_window;')
    old = 'static void tb_ring_poll_complete(struct tb_ring *r) { atomic_fetch_add(&poll_completes, 1); r->masked = false; }'
    assert old in harness
    # MMIO is a double; all ownership/locking decisions below are real source.
    funcs = '\n'.join(core_function(nhi, n) for n in ('tb_ring_poll_complete', 'tb_ring_poll_begin', 'tb_ring_poll_end'))
    harness = harness.replace(old, 'static void __ring_interrupt_mask(struct tb_ring *r, bool mask) { r->masked = mask; if (!mask) atomic_fetch_add(&poll_completes, 1); }\n' + funcs)
    harness = harness.replace('r->running = false;', 'r->running = false; r->poll_window = false;')
    (tmp / 'stream_harness.h').write_text(harness)
    cases = (tmp / 'stream_cases.h').read_text()
    cases = cases.replace('h->tb.nhi = &h->nhi;', 'spin_lock_init(&h->nhi.lock); h->tb.nhi = &h->nhi;')
    cases = cases.replace('mutex_init(&s->lock); mutex_init(&s->rx_lock);', '''mutex_init(&s->lock); mutex_init(&s->rx_lock);
            spin_lock_init(&s->window_lock); s->rx_window_us = 1000;
            hrtimer_setup(&s->window_timer, tbstream_dev_window_expire, CLOCK_MONOTONIC, HRTIMER_MODE_ABS_HARD);''')
    cases = cases.replace('int main(int argc, char **argv)', '#include "stream_hybrid_cases.h"\n\nint main(int argc, char **argv)')
    cases = cases.replace('cases[] = {', 'cases[] = {\n        { "hybrid_policy_and_lifetime", hybrid_policy_and_lifetime },\n        { "hybrid_config_and_exclusive_open", hybrid_config_and_exclusive_open },\n        { "hybrid_ioctl_faults", hybrid_ioctl_faults },\n        { "hybrid_core_guards", hybrid_core_guards },\n        { "hybrid_expire_end_race", hybrid_expire_end_race },')
    (tmp / 'stream_cases.h').write_text(cases)
    functions = host.functions(stream)
    i = functions.index('tbstream_dev_stop')
    functions[i:i] = ('tbstream_dev_window_stop', 'tbstream_dev_window_expire', 'tbstream_dev_window_command')
    functions += ['tbstream_dev_fops_ioctl', 'tbstream_dev_config_busy', 'tbstream_dev_rx_window_bytes_store', 'tbstream_dev_rx_window_us_store', 'tbstream_dev_busy_poll_store', 'tbstream_dev_rx_busy_poll_store', 'tbstream_dev_rx_repost_batch_store', 'tbstream_dev_tx_submit_batch_store']
    enum = re.search(r'^enum tbstream_frame_pdf \{\n.*?^\};\n', stream, re.M | re.S).group(0)
    parts = ['#include "stream_harness.h"\n', enum,
             ''.join(re.findall(r'^#define TBSTREAM_DEV_[A-Z_]+\t+\d+\n', stream, re.M)),
             ''.join(re.findall(r'^#define TBSTREAM_DEV_(?:CHUNK_MAX_ORDER|MAX_CHUNKS|CONTIG_TX|CONTIG_RX)\t+.*\n', stream, re.M)),
             ''.join(re.findall(r'^#define TBSTREAM_TX_[A-Z_]+\t+.*\n', stream, re.M))]
    parts += [host.extract(stream, 'struct', n) for n in host.STRUCTS]
    parts += [host.extract(stream, 'function', n) for n in functions]
    parts += ['#include "stream_cases.h"\n']
    (tmp / 'unit.c').write_text('\n'.join(parts))
    # 0049 is only in the experimental series, so the 0050/0051 cases apply too.
    command = [os.environ.get('CC', 'cc'), '-DUSB4_EXPERIMENTAL', '-std=gnu11', '-g', '-O1', '-Wall', '-Wextra', '-Werror',
               '-Wno-unused-function', '-Wno-unused-parameter', '-Wno-sign-compare',
               '-fno-omit-frame-pointer', '-fno-pie', '-no-pie', '-fsanitize=address,undefined', '-pthread',
               '-I', str(tmp), '-I', str(tree / 'include/uapi'), str(tmp / 'unit.c'), '-o', str(tmp / 'run')]
    result = subprocess.run(command, capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr
    return tmp / "run"


class HybridWindowTests(unittest.TestCase):
    def test_actual_functions_asan(self):
        source = os.environ.get('USB4_SOURCE_TREE')
        if not source:
            self.skipTest('USB4_SOURCE_TREE required')
        tree = Path(source)
        stream = (tree / 'drivers/thunderbolt/stream.c').read_text()
        if 'tbstream_dev_window_command' not in stream:
            self.skipTest('0049 source required')
        nhi = (tree / 'drivers/thunderbolt/nhi.c').read_text()
        with tempfile.TemporaryDirectory(prefix='hybrid-asan-') as directory:
            binary = build_hybrid_unit(tree, Path(directory))
            all_cases = list(host.CASES + host.EXPERIMENTAL_CASES) + ['hybrid_policy_and_lifetime', 'hybrid_config_and_exclusive_open', 'hybrid_ioctl_faults', 'hybrid_core_guards', 'hybrid_expire_end_race']
            for name in all_cases:
                with self.subTest(case=name):
                    result = subprocess.run([str(binary), name], capture_output=True, text=True, timeout=30,
                                            env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=1:abort_on_error=1'})
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    self.assertIn('PASS ' + name, result.stdout)
            print(f'0049 ASAN/UBSAN actual core + host: {len(all_cases)} cases PASS')


if __name__ == '__main__':
    unittest.main()
