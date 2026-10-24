#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""tools/stream_mem_bench.c over named pipes and the run_stream_bench aggregate.

No /dev/tbstreamN is opened: FIFOs stand in for the device, so this checks the
harness (pattern, verification, accounting), not the driver or the link.
"""
import contextlib
import io
import time
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import threading
import unittest

REPO = Path(__file__).resolve().parents[1]
CC = os.environ.get('CC', 'cc')
spec = importlib.util.spec_from_file_location('run_stream_bench', REPO / 'tools/lab/run_stream_bench.py')
bench = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bench)


@unittest.skipUnless(shutil.which(CC) and hasattr(os, 'mkfifo'), 'needs a C compiler and FIFOs')
class StreamMemBenchTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix='stream-bench-')
        cls.binary = Path(cls.tmp.name) / 'stream_mem_bench'
        subprocess.run([CC, '-std=gnu11', '-O2', '-Wall', '-fsanitize=address,undefined',
                        '-fno-sanitize-recover=all', str(REPO / 'tools/stream_mem_bench.c'),
                        '-o', str(cls.binary)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def transfer(self, total, block, corrupt_at=None):
        with tempfile.TemporaryDirectory() as tmp:
            a, b = Path(tmp) / 'a', Path(tmp) / 'b'
            os.mkfifo(a)
            os.mkfifo(b)

            def relay():  # sender -> a -> (optional bit flip) -> b -> receiver
                seen = 0
                with open(a, 'rb') as src, open(b, 'wb') as dst:
                    while chunk := src.read(65536):
                        if corrupt_at is not None and seen <= corrupt_at < seen + len(chunk):
                            chunk = bytearray(chunk)
                            chunk[corrupt_at - seen] ^= 1
                        seen += len(chunk)
                        dst.write(chunk)

            thread = threading.Thread(target=relay)
            thread.start()
            recv = subprocess.Popen([str(self.binary), 'recv', str(b), str(total), str(block), '1'],
                                    stdout=subprocess.PIPE, text=True)
            send = subprocess.run([str(self.binary), 'send', str(a), str(total), str(block), '0'],
                                  capture_output=True, text=True, timeout=60)
            out, _ = recv.communicate(timeout=60)
            thread.join(timeout=60)
            return send, recv.returncode, json.loads(out.splitlines()[-1])

    def test_verified_transfer(self):
        for total, block in ((1 << 20, 65536), (3 * 65536 + 13, 4096)):
            with self.subTest(total=total, block=block):
                send, code, result = self.transfer(total, block)
                self.assertEqual((send.returncode, code), (0, 0), send.stderr)
                self.assertEqual((result['mode'], result['bytes'], result['bad_chunks'],
                                  result['verification']), ('recv', total, 0, 1))
                self.assertGreater(result['first_bytes_excluded'], 0)

    def test_single_flipped_bit_is_detected(self):
        send, code, result = self.transfer(1 << 20, 65536, corrupt_at=777_777)
        self.assertEqual(code, 7)
        self.assertEqual(result['bad_chunks'], 1)

    def test_diagnostic_counts_preserve_verified_bytes(self):
        binary = Path(self.tmp.name) / 'stream_mem_bench_diagnostic'
        subprocess.run([CC, '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror',
                        '-DUSB4_BENCH_DIAGNOSTIC', str(REPO / 'tools/stream_mem_bench.c'),
                        '-o', str(binary)], check=True)
        self.binary = binary
        total = 3 * 65536 + 13
        send, code, result = self.transfer(total, 65536)
        self.assertEqual((send.returncode, code), (0, 0))
        for row in (json.loads(send.stdout.splitlines()[-1]), result):
            counters = row['io_stats']
            self.assertEqual(counters['bytes'], total)
            self.assertEqual(sum(counters['bytes_le_4k_16k_64k_256k_1m_over_1m']), counters['positive'])
            self.assertEqual(counters['calls'], counters['positive'] + counters['eagain'] + counters['eintr'])
            self.assertLessEqual(counters['short'], counters['positive'])

    def test_usage_errors(self):
        for args in ([], ['send', '/dev/null', '0', '4096', '1'],
                     ['send', '/dev/null', '100', '0', '1'],
                     ['send', '/dev/null', '100', str((64 << 20) + 1), '1']):
            with self.subTest(args=args):
                self.assertEqual(subprocess.run([str(self.binary), *args],
                                                capture_output=True).returncode, 2)


class AggregateTests(unittest.TestCase):
    def recv(self, start, end, nbytes=8 << 30, first=1 << 20, bad=0):
        return dict(mode='recv', bytes=nbytes, first_bytes_excluded=first, start_monotonic=start,
                    end_monotonic=end, gbps=1.0, verification=1, bad_chunks=bad)

    def test_common_window(self):
        result = bench.aggregate([self.recv(10.0, 20.0), self.recv(10.5, 21.0)])
        self.assertAlmostEqual(result['window_seconds'], 11.0)
        self.assertEqual(result['payload_bytes'], 2 * ((8 << 30) - (1 << 20)))
        self.assertAlmostEqual(result['gbps'], result['payload_bytes'] * 8e-9 / 11.0)

    def test_invalid_streams_void_the_run(self):
        self.assertIsNone(bench.aggregate([self.recv(0, 1), self.recv(0, 1, bad=1)]))
        self.assertIsNone(bench.aggregate([self.recv(0, 1), None]))
        self.assertIsNone(bench.aggregate([]))

    def test_plan_pins_bounds_and_marks_every_process(self):
        args = bench.argparse.Namespace(peer='node-b', bench=Path('/opt/b'), bytes=1 << 30,
                                        block=1 << 20, direction='send', timeout=600, run_id='r1',
                                        ssh='ssh -o BatchMode=yes',
                                        pair=[bench.parse_pair('/dev/tbstream0:/dev/tbstream2:2:4')])
        plan = bench.plan(args)
        stream = plan['streams'][0]
        self.assertEqual(stream['marker'], 'usb4bench-r1-0')
        self.assertEqual(stream['local'][:4], ['bash', '-c', 'exec -a "$0" timeout '
                                               '--kill-after=10 600 "$@"', 'usb4bench-r1-0'])
        self.assertEqual(stream['local'][4:], ['taskset', '-c', '2', '/opt/b', 'send',
                                               '/dev/tbstream0', str(1 << 30), str(1 << 20), '1'])
        self.assertEqual(stream['remote'][:4], ['ssh', '-o', 'BatchMode=yes', 'node-b'])
        self.assertIn('timeout --kill-after=10 600', stream['remote'][-1])
        self.assertIn('taskset -c 4 /opt/b recv /dev/tbstream2', stream['remote'][-1])
        self.assertEqual((plan['receiver'], plan['timeout_seconds']), ('peer', 600))
        self.assertEqual(bench.parse_size('8G'), 8 << 30)
        with self.assertRaises(bench.argparse.ArgumentTypeError):
            bench.parse_pair('/dev/tbstream0:/dev/tbstream0')


FAKE_SSH = """#!/bin/sh
# Stand-in for ssh: drop options and the host, run the command locally.
while [ "$1" = -o ]; do shift 2; done
shift
exec sh -c "$*"
"""


@unittest.skipUnless(shutil.which(CC) and hasattr(os, 'mkfifo') and shutil.which('timeout')
                     and shutil.which('taskset') and shutil.which('pkill'),
                     'needs cc, FIFOs, timeout, taskset and pkill')
class RunnerEndToEndTests(unittest.TestCase):
    """The whole runner with a local stand-in for ssh; FIFOs replace the devices."""

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory(prefix='bench-run-')
        self.addCleanup(self._tmp.cleanup)
        self.root = Path(self._tmp.name)
        self.ssh = self.root / 'fake-ssh'
        self.ssh.write_text(FAKE_SSH)
        self.ssh.chmod(0o755)

    def run_bench(self, bench_path, pairs, *extra):
        log = self.root / 'kernel.log'
        log.write_text('')
        proc = self.root / 'proc'
        proc.mkdir(exist_ok=True)
        args = ['--peer', 'peer', '--out', str(self.root / 'out'), '--bench', str(bench_path),
                '--ssh', str(self.ssh),
                '--diagnose-args', f'--kernel-log {log} --proc-root {proc}', *extra]
        for pair in pairs:
            args += ['--pair', pair]
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            status = bench.main(args)
        return status, json.loads((self.root / 'out/result.json').read_text())

    def test_complete_run_writes_valid_result(self):
        binary = self.root / 'stream_mem_bench'
        subprocess.run([CC, '-O2', str(REPO / 'tools/stream_mem_bench.c'), '-o', str(binary)],
                       check=True)
        pairs = []
        for i in range(2):
            fifo = self.root / f'dev{i}'
            os.mkfifo(fifo)
            pairs.append(f'{fifo}:{fifo}:0:0')
        status, result = self.run_bench(binary, pairs, '--bytes', '4M', '--block', '64K',
                                        '--timeout', '60')
        self.assertEqual(status, 0, result)
        self.assertEqual((result['status'], result['valid']), ('complete', True))
        self.assertEqual(result['aggregate']['streams'], 2)
        self.assertEqual(set(result['exit_codes'].values()), {0})
        for name in ('plan.json', 'local-identity.json', 'peer-identity.json', 'local-0.log',
                     'peer-1.log'):
            self.assertTrue((self.root / 'out' / name).exists(), name)

    def test_timeout_stops_both_ends_and_still_writes_result(self):
        hang = self.root / 'hang'
        hang.write_text('#!/bin/sh\nexec sleep 1000\n')
        hang.chmod(0o755)
        started = time.monotonic()
        status, result = self.run_bench(hang, ['/dev/null:/dev/null:0:0'], '--timeout', '2')
        self.assertEqual(status, 1)
        self.assertEqual(result['status'], 'timeout')
        self.assertFalse(result['valid'])
        self.assertIsNone(result['aggregate'])
        self.assertEqual(set(result['exit_codes'].values()), {'timeout'})
        self.assertLess(time.monotonic() - started, 60)
        leftover = subprocess.run(['pgrep', '-f', f'usb4bench-{result["run_id"]}'],
                                  capture_output=True, text=True)
        self.assertEqual(leftover.stdout, '')

    def test_refused_run_records_why(self):
        diag_fail = self.root / 'fake-ssh-refuse'
        diag_fail.write_text(FAKE_SSH.replace('exec sh -c "$*"',
                                              'case "$*" in *usb4_diagnose*) echo budget >&2; '
                                              'exit 1;; esac\nexec sh -c "$*"'))
        diag_fail.chmod(0o755)
        self.ssh = diag_fail
        status, result = self.run_bench(Path('/bin/true'), ['/dev/null:/dev/null:0:0'])
        self.assertEqual((status, result['status'], result['valid']), (1, 'refused', False))
        self.assertIn('peer: ring budget check failed', result['reason'])

    def test_preflight_is_fail_closed_with_logs(self):
        src = (REPO / 'tools/lab/run_stream_bench.py').read_text()
        self.assertIn("'--require-budget', '--logs'", src)
        # An unreadable kernel log on the peer refuses the run.
        status, result = self.run_bench(Path('/bin/true'), ['/dev/null:/dev/null:0:0'],
                                        '--diagnose-args', f'--kernel-log {self.root}/missing.log')
        self.assertEqual((status, result['status']), (1, 'refused'))
        self.assertIn('kernel log unavailable', result['reason'])

if __name__ == '__main__':
    unittest.main(verbosity=2)
