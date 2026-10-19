#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""tools/stream_mem_bench.c over named pipes and the run_stream_bench aggregate.

No /dev/tbstreamN is opened: FIFOs stand in for the device, so this checks the
harness (pattern, verification, accounting), not the driver or the link.
"""
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

    def test_plan_pins_every_process_and_verifies(self):
        args = bench.argparse.Namespace(peer='cluster2', bench=Path('/opt/b'), bytes=1 << 30,
                                        block=1 << 20, direction='send',
                                        pair=[bench.parse_pair('/dev/tbstream0:/dev/tbstream2:2:4')])
        plan = bench.plan(args)
        stream = plan['streams'][0]
        self.assertEqual(stream['local'], ['taskset', '-c', '2', '/opt/b', 'send', '/dev/tbstream0',
                                           str(1 << 30), str(1 << 20), '1'])
        self.assertIn('taskset -c 4 /opt/b recv /dev/tbstream2', stream['remote'][-1])
        self.assertEqual(plan['receiver'], 'peer')
        self.assertEqual(bench.parse_size('8G'), 8 << 30)
        with self.assertRaises(bench.argparse.ArgumentTypeError):
            bench.parse_pair('/dev/tbstream0:/dev/tbstream0')


if __name__ == '__main__':
    unittest.main(verbosity=2)
