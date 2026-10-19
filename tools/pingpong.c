// SPDX-License-Identifier: GPL-2.0-only
// Round-trip latency over one /dev/tbstreamN: client writes `size` bytes,
// server echoes them back.
// usage: pingpong server|client dev size iters warmup [timeout_ms]
//
// Every payload carries a position/iteration pattern that both ends verify in
// full. Each round trip must finish within timeout_ms (default 5000); the
// server waits up to max(60 s, timeout_ms) for the first message. Exit codes:
// 2 usage, 4 open/alloc, 5 I/O error or EOF, 6 payload mismatch, 7 timeout.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define PP_MAX_SIZE (64UL << 20)
#define PP_MAX_ITERS 10000000UL
#define PP_MAX_TIMEOUT_MS 3600000UL
#define PP_START_TIMEOUT_MS 60000UL

enum { PP_OK = 0, PP_USAGE = 2, PP_SETUP = 4, PP_IO = 5, PP_MISMATCH = 6, PP_TIMEOUT = 7 };

static volatile sig_atomic_t expired;

static void on_alarm(int sig)
{
	(void)sig;
	expired = 1;
}

static int install_alarm(void)
{
	struct sigaction sa;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_alarm;	/* no SA_RESTART: blocked I/O returns EINTR */
	sigemptyset(&sa.sa_mask);
	return sigaction(SIGALRM, &sa, NULL);
}

static void arm(unsigned long ms)
{
	struct itimerval t = { { 0, 0 }, { (time_t)(ms / 1000), (suseconds_t)(ms % 1000) * 1000 } };

	expired = 0;
	setitimer(ITIMER_REAL, &t, NULL);
}

static void disarm(void)
{
	struct itimerval t = { { 0, 0 }, { 0, 0 } };

	setitimer(ITIMER_REAL, &t, NULL);
}

/* Parse a decimal/hex unsigned value in [lo, hi]; reject sign, junk, overflow. */
static int parse_ulong(const char *s, unsigned long lo, unsigned long hi, unsigned long *out)
{
	char *end;
	unsigned long v;

	if (!s || !*s || *s == '-' || *s == '+')
		return -1;
	errno = 0;
	v = strtoul(s, &end, 0);
	if (errno || *end || v < lo || v > hi)
		return -1;
	*out = v;
	return 0;
}

/* Byte `pos` of iteration `iter`: differs per position and per iteration. */
static void pp_fill(unsigned char *buf, size_t size, uint64_t iter)
{
	for (size_t pos = 0; pos < size; pos++) {
		uint64_t v = (iter * UINT64_C(0x9e3779b97f4a7c15)) ^ ((uint64_t)(pos / 8) * UINT64_C(0xbf58476d1ce4e5b9));
		buf[pos] = (unsigned char)(v >> (8 * (pos % 8)));
	}
}

/* Index of the first wrong byte, or -1 if the whole payload matches. */
static long pp_check(const unsigned char *buf, const unsigned char *want, size_t size)
{
	for (size_t i = 0; i < size; i++)
		if (buf[i] != want[i])
			return (long)i;
	return -1;
}

/* Move exactly n bytes; retry EINTR until the armed deadline expires. */
static int full(int fd, unsigned char *b, size_t n, int wr)
{
	while (n) {
		ssize_t r = wr ? write(fd, b, n) : read(fd, b, n);

		if (r < 0) {
			if (errno == EINTR) {
				if (expired)
					return PP_TIMEOUT;
				continue;
			}
			return PP_IO;
		}
		if (r == 0) {
			errno = ENODATA;	/* EOF / no progress */
			return PP_IO;
		}
		b += r;
		n -= (size_t)r;
	}
	return PP_OK;
}

static double now(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec * 1e-9;
}

static int cmp(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;

	return (x > y) - (x < y);
}

struct pp_args {
	int server;
	const char *dev;
	size_t size;
	unsigned long iters, warm, timeout_ms;
};

static int pp_parse(int argc, char **argv, struct pp_args *a)
{
	unsigned long v;

	if (argc != 6 && argc != 7)
		return -1;
	if (!strcmp(argv[1], "server"))
		a->server = 1;
	else if (!strcmp(argv[1], "client"))
		a->server = 0;
	else
		return -1;
	a->dev = argv[2];
	if (parse_ulong(argv[3], 1, PP_MAX_SIZE, &v))
		return -1;
	a->size = v;
	if (parse_ulong(argv[4], 1, PP_MAX_ITERS, &a->iters) ||
	    parse_ulong(argv[5], 0, PP_MAX_ITERS, &a->warm))
		return -1;
	a->timeout_ms = 5000;
	if (argc == 7 && parse_ulong(argv[6], 1, PP_MAX_TIMEOUT_MS, &a->timeout_ms))
		return -1;
	return 0;
}

/* Run both roles over an open fd; the client prints one JSON line of statistics. */
static int pp_run(int fd, const struct pp_args *a, FILE *out)
{
	size_t alloc = (a->size + 4095) & ~(size_t)4095;
	unsigned char *tx = aligned_alloc(4096, alloc), *rx = aligned_alloc(4096, alloc);
	double *rtt = a->server ? NULL : calloc(a->iters, sizeof(*rtt));
	unsigned long total = a->warm + a->iters;
	int ret = PP_OK;

	if (!tx || !rx || (!a->server && !rtt)) {
		ret = PP_SETUP;
		goto out;
	}
	for (unsigned long i = 0; i < total; i++) {
		unsigned long limit = a->timeout_ms;

		pp_fill(tx, a->size, i);
		if (a->server) {
			if (!i && limit < PP_START_TIMEOUT_MS)
				limit = PP_START_TIMEOUT_MS;
			arm(limit);
			ret = full(fd, rx, a->size, 0);
			disarm();
			if (ret)
				break;
			if (pp_check(rx, tx, a->size) >= 0) {
				ret = PP_MISMATCH;
				break;
			}
			arm(a->timeout_ms);
			ret = full(fd, rx, a->size, 1);
			disarm();
			if (ret)
				break;
			continue;
		}
		memset(rx, 0, a->size);
		arm(limit);
		double t0 = now();
		ret = full(fd, tx, a->size, 1);
		if (!ret)
			ret = full(fd, rx, a->size, 0);
		double dt = (now() - t0) * 1e6;
		disarm();
		if (ret)
			break;
		long bad = pp_check(rx, tx, a->size);
		if (bad >= 0) {
			fprintf(stderr, "payload mismatch: iteration %lu byte %ld\n", i, bad);
			ret = PP_MISMATCH;
			break;
		}
		if (i >= a->warm)
			rtt[i - a->warm] = dt;
	}
	if (ret == PP_TIMEOUT)
		fprintf(stderr, "timeout: round trip exceeded %lu ms\n", a->timeout_ms);
	if (!ret && !a->server) {
		unsigned long n = a->iters;
		double sum = 0;

		for (unsigned long i = 0; i < n; i++)
			sum += rtt[i];
		qsort(rtt, n, sizeof(*rtt), cmp);
		fprintf(out, "{\"size\":%zu,\"n\":%lu,\"verified_bytes_per_iter\":%zu,\"min\":%.2f,"
			"\"p50\":%.2f,\"p90\":%.2f,\"p99\":%.2f,\"max\":%.2f,\"mean\":%.2f}\n",
			a->size, n, a->size, rtt[0], rtt[n / 2], rtt[n * 9 / 10],
			rtt[n * 99 / 100], rtt[n - 1], sum / n);
	}
out:
	free(tx);
	free(rx);
	free(rtt);
	return ret;
}

#ifndef PINGPONG_NO_MAIN
int main(int argc, char **argv)
{
	struct pp_args a;
	int fd, ret;

	if (pp_parse(argc, argv, &a)) {
		fprintf(stderr, "usage: pingpong server|client dev size iters warmup [timeout_ms]\n"
			"  size 1..%lu bytes, iters 1..%lu, warmup 0..%lu, timeout_ms 1..%lu (default 5000)\n",
			PP_MAX_SIZE, PP_MAX_ITERS, PP_MAX_ITERS, PP_MAX_TIMEOUT_MS);
		return PP_USAGE;
	}
	if (install_alarm()) {
		perror("sigaction");
		return PP_SETUP;
	}
	fd = open(a.dev, O_RDWR);
	if (fd < 0) {
		perror("open");
		return PP_SETUP;
	}
	printf("READY\n");
	fflush(stdout);
	ret = pp_run(fd, &a, stdout);
	if (ret == PP_IO)
		perror("transfer");
	close(fd);
	return ret;
}
#endif
