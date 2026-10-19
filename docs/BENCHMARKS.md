# Reproducible USB4STREAM measurements

The bandwidth and latency figures in the README are **lab observations** from
2026-10-17 (bandwidth) and the 0010 work (latency). Their raw logs stayed on
the machines; this repository does not contain them, so they cannot be
re-audited from here. In particular the "~9–11 Gb/s per ring pair" figure is
an observation of these two machines, not a property of the driver. Any new
figure must come with the records listed below.

## Harnesses

| Tool | Measures | Verification |
|---|---|---|
| [`tools/stream_mem_bench.c`](../tools/stream_mem_bench.c) | One-direction host-memory throughput of one stream | Receiver checks **every byte** against the sender's position-dependent pattern; `bad_chunks` counts mismatching reads; exit 7 if any |
| [`tools/lab/run_stream_bench.py`](../tools/lab/run_stream_bench.py) | N streams at once, both hosts | Records identities, refuses to run if the ring budget fails, keeps all raw logs, voids the run if any stream fails or is unverified |
| [`tools/pingpong.c`](../tools/pingpong.c) | Round-trip latency of one stream | Both ends verify the full payload (position and iteration pattern); each round trip bounded by `timeout_ms` |

`stream_mem_bench.c` is the harness that produced the README bandwidth table
(written for this lab on 2026-10-15); only a header comment was added. Build
both tools on each host with the same compiler and flags and record their
sha256:

```sh
cc -O2 -Wall -o ~/bin/stream_mem_bench tools/stream_mem_bench.c
cc -O2 -Wall -o ~/bin/pingpong tools/pingpong.c
```

## Before every run (both hosts)

1. Kernel and modules: `uname -r` must be the intended lab build;
   `cat /sys/module/{thunderbolt,thunderbolt_stream}/srcversion`.
2. Ring budget: `python3 tools/usb4_diagnose.py --require-budget` must exit 0
   (no thunderbolt-net service bound, modprobe policy present, at most two
   stream consumers per controller).
3. Link: `python3 tools/lab/usb4stream_describe.py --summary` must show every
   cable used at the intended speed (Gen3 × 2 = 40 Gb/s). A cable that trained
   lower, or with an unknown speed, invalidates a comparison with earlier runs.
4. Stream settings: the configfs attributes of each stream (`ring_size`,
   `throttling`, `busy_poll`, `rx_napi`, `spin_us`, `irq_cpu`) and the module
   parameters; `run_stream_bench.py` stores them.
5. No other stream users, no `gpu_direct` session on the streams under test.

## Bandwidth run

The benchmark binary and this checkout must be at the same paths on both hosts.
Example: four streams, both cables, this host sends 8 GiB per stream in 1 MiB
writes, each process pinned to its own CPU on both ends:

```sh
tools/lab/run_stream_bench.py --peer cluster2 --out ~/bench/$(date +%Y%m%d-%H%M)-4x \
    --bench ~/bin/stream_mem_bench --bytes 8G --block 1M \
    --pair /dev/tbstream0:/dev/tbstream0:2:2 --pair /dev/tbstream1:/dev/tbstream1:4:4 \
    --pair /dev/tbstream2:/dev/tbstream2:6:6 --pair /dev/tbstream3:/dev/tbstream3:8:8
```

Use `usb4stream-describe --merge` (or the layout file) to pick device pairs:
`tbstreamN` numbers are per host and pair by cable and stream name, not by
number. Run `--dry-run` first to review the exact commands.

The output directory keeps `plan.json` (commands, sizes, affinities),
`local-identity.json` and `peer-identity.json` (release, cmdline, module
srcversions and parameters, configfs attributes, topology, ring budget, binary
hash, driver `git describe`), the raw log of every sender and receiver, and
`result.json`:

* per stream: both ends' JSON lines (bytes, seconds, Gb/s, CPU seconds,
  monotonic start/end, bytes excluded at start, bad chunks);
* aggregate: `sum(bytes - first_bytes_excluded) * 8 / (max(end) - min(start))`
  over all receivers (one host, one `CLOCK_MONOTONIC`), i.e. the common
  receiver-clock window;
* `valid`: false if any process failed, any receiver did not verify or any
  chunk was bad. Invalid runs are kept, not discarded.

Repeat each configuration several times (at least 3), report every run (or
min/median/max) and run the opposite direction from the other host. Publish
the result directories together with any new number.

## Latency run

```sh
# peer, first
taskset -c 2 ~/bin/pingpong server /dev/tbstreamM 64 2000 200 5000
# this host
taskset -c 2 ~/bin/pingpong client /dev/tbstreamN 64 2000 200 5000
```

Arguments: size (1..64 MiB), iterations (≥ 1), warm-up, per-round-trip
timeout in ms. The client prints min/p50/p90/p99/max/mean in µs. Record the
same identity data as for bandwidth (the `rx_napi`, `spin_us`, `throttling`,
`busy_poll` and `irq_cpu` settings dominate the result) and the exit status of
both ends (6 = payload mismatch, 7 = timeout).
