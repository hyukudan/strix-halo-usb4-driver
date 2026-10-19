#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""tools/pingpong.c: argument validation, full-payload verification and deadlines.

The device is replaced by a socketpair; nothing opens /dev/tbstreamN.
"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[1]
SOURCE = REPO / 'tools/pingpong.c'
CC = os.environ.get('CC', 'cc')
FLAGS = ['-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
         '-fsanitize=address,undefined', '-fno-sanitize-recover=all']

HARNESS = r'''
#define PINGPONG_NO_MAIN
#include "%s"
#include <sys/socket.h>
#include <sys/wait.h>

/* mode: 0 echo, 1 corrupt one middle byte of iteration 3, 2 stall forever, 3 close early */
static void fake_server(int fd, size_t size, unsigned long total, int mode)
{
	unsigned char *b = malloc(size);
	for (unsigned long i = 0; i < total; i++) {
		if (full(fd, b, size, 0))
			_exit(0);
		if (mode == 1 && i == 3)
			b[size / 2] ^= 0x40;
		if (mode == 2)
			pause();
		if (mode == 3 && i == 2)
			_exit(0);
		if (full(fd, b, size, 1))
			_exit(0);
	}
	free(b);
	_exit(0);
}

int main(int argc, char **argv)
{
	struct pp_args a = { .server = 0, .dev = "socketpair" };
	int sv[2], mode = atoi(argv[1]), status, ret;
	FILE *null = fopen("/dev/null", "w");

	a.size = strtoul(argv[2], 0, 0);
	a.iters = 8;
	a.warm = 2;
	a.timeout_ms = 300;
	if (argc > 3) {	/* real server role on the other end */
		a.iters = 5;
	}
	if (install_alarm() || socketpair(AF_UNIX, SOCK_STREAM, 0, sv))
		return 90;
	pid_t pid = fork();
	if (!pid) {
		close(sv[0]);
		if (argc > 3) {
			struct pp_args s = a;
			s.server = 1;
			_exit(pp_run(sv[1], &s, null));
		}
		fake_server(sv[1], a.size, a.iters + a.warm, mode);
	}
	close(sv[1]);
	ret = pp_run(sv[0], &a, stdout);
	close(sv[0]);
	if (mode == 2)
		kill(pid, SIGKILL);
	waitpid(pid, &status, 0);
	if (argc > 3 && (!WIFEXITED(status) || WEXITSTATUS(status)))
		return 91;
	return ret;
}
'''


@unittest.skipUnless(shutil.which(CC), 'C compiler not available')
class PingPongTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix='pingpong-')
        tmp = Path(cls.tmp.name)
        cls.binary = tmp / 'pingpong'
        subprocess.run([CC, *FLAGS, str(SOURCE), '-o', str(cls.binary)], check=True)
        harness = tmp / 'harness.c'
        harness.write_text(HARNESS % SOURCE.as_posix())
        cls.harness = tmp / 'harness'
        subprocess.run([CC, *FLAGS, '-Wno-unused-function', str(harness), '-o', str(cls.harness)],
                       check=True)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def cli(self, *args):
        return subprocess.run([str(self.binary), *args], capture_output=True, text=True, timeout=30)

    def test_rejects_invalid_arguments_before_opening(self):
        bad = [
            ('client', '/nonexistent', '64', '0', '0'),            # 0 iterations: stats OOB
            ('client', '/nonexistent', '0', '10', '0'),            # size 0: buf[size-1]
            ('client', '/nonexistent', '-1', '10', '0'),
            ('client', '/nonexistent', '64', '-5', '0'),
            ('client', '/nonexistent', '64', '10', '-1'),
            ('client', '/nonexistent', '64x', '10', '0'),
            ('client', '/nonexistent', '99999999999999999999', '10', '0'),
            ('client', '/nonexistent', str((64 << 20) + 1), '10', '0'),
            ('client', '/nonexistent', '64', '10000001', '0'),
            ('client', '/nonexistent', '64', '10', '0', '0'),      # timeout 0
            ('echo', '/nonexistent', '64', '10', '0'),
            ('client', '/nonexistent', '64', '10'),
        ]
        for args in bad:
            with self.subTest(args=args):
                result = self.cli(*args)
                self.assertEqual(result.returncode, 2, result.stderr)
                self.assertIn('usage', result.stderr)

    def test_valid_arguments_reach_open(self):
        result = self.cli('client', '/nonexistent/tbstream0', '64', '1', '0', '100')
        self.assertEqual(result.returncode, 4)

    def run_harness(self, mode, size, *extra):
        return subprocess.run([str(self.harness), str(mode), str(size), *extra],
                              capture_output=True, text=True, timeout=60)

    def test_echo_passes_and_reports_stats(self):
        for size in (1, 7, 64, 4096, 65537):
            with self.subTest(size=size):
                result = self.run_harness(0, size)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn(f'"size":{size},"n":8', result.stdout)

    def test_real_server_role_verifies_and_echoes(self):
        result = self.run_harness(0, 4096, 'server')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('"n":5', result.stdout)

    def test_middle_byte_corruption_is_detected(self):
        result = self.run_harness(1, 4096)
        self.assertEqual(result.returncode, 6)
        self.assertIn('iteration 3 byte 2048', result.stderr)

    def test_stalled_peer_hits_deadline(self):
        result = self.run_harness(2, 64)
        self.assertEqual(result.returncode, 7)
        self.assertIn('timeout', result.stderr)

    def test_peer_eof_is_an_error(self):
        result = self.run_harness(3, 64)
        self.assertEqual(result.returncode, 5)

    def test_pattern_changes_per_iteration_and_position(self):
        src = SOURCE.read_text()
        self.assertIn('pp_fill(tx, a->size, i)', src)
        self.assertNotIn('buf[size - 1]', src)


if __name__ == '__main__':
    unittest.main(verbosity=2)
