// Round-trip latency over one /dev/tbstreamN: client writes `size` bytes,
// server echoes them back. usage: pingpong server|client dev size iters warmup
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int full(int fd, char *b, size_t n, int wr)
{
	while (n) {
		ssize_t r = wr ? write(fd, b, n) : read(fd, b, n);
		if (r <= 0)
			return -1;
		b += r;
		n -= r;
	}
	return 0;
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

int main(int argc, char **argv)
{
	if (argc != 6) {
		fprintf(stderr, "usage: pingpong server|client dev size iters warmup\n");
		return 2;
	}
	int server = !strcmp(argv[1], "server");
	size_t size = strtoul(argv[3], 0, 0);
	int iters = atoi(argv[4]), warm = atoi(argv[5]);
	char *buf = aligned_alloc(4096, (size + 4095) & ~4095UL);
	double *rtt = calloc(iters, sizeof(*rtt));
	int fd = open(argv[2], O_RDWR);
	if (fd < 0 || !buf || !rtt) {
		perror("open");
		return 4;
	}
	printf("READY\n");
	fflush(stdout);
	for (int i = 0; i < warm + iters; i++) {
		if (server) {
			if (full(fd, buf, size, 0) || full(fd, buf, size, 1))
				return 5;
			continue;
		}
		memset(buf, i & 0xff, size);
		double t0 = now();
		if (full(fd, buf, size, 1) || full(fd, buf, size, 0))
			return 5;
		double dt = (now() - t0) * 1e6;
		if (buf[0] != (char)(i & 0xff) || buf[size - 1] != (char)(i & 0xff))
			return 6;
		if (i >= warm)
			rtt[i - warm] = dt;
	}
	if (!server) {
		double sum = 0;
		for (int i = 0; i < iters; i++)
			sum += rtt[i];
		qsort(rtt, iters, sizeof(*rtt), cmp);
		printf("{\"size\":%zu,\"n\":%d,\"min\":%.2f,\"p50\":%.2f,\"p90\":%.2f,\"p99\":%.2f,\"max\":%.2f,\"mean\":%.2f}\n",
		       size, iters, rtt[0], rtt[iters / 2], rtt[iters * 9 / 10],
		       rtt[iters * 99 / 100], rtt[iters - 1], sum / iters);
	}
	close(fd);
	return 0;
}
