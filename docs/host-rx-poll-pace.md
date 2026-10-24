# Paced host RX descriptor polls (experimental, 0050)

Patch 0050 adds the per-stream configfs attribute `rx_poll_pace_ns`
(0..10000, default 0). It only acts on polled host RX rings: `busy_poll=1` or
`rx_busy_poll=1`. With interrupt/NAPI RX (including 0049 windows), GPU-direct or
the default 0 the code path is unchanged. Configure it on a closed stream like
the other ring attributes (EBUSY otherwise, ERANGE above 10000).

When a polled RX read finds no new completion, the stream records
`now + rx_poll_pace_ns`; until then later reads do not touch the descriptor ring
and return -EAGAIN (or keep looping, for blocking readers) unless frames reaped
earlier are still waiting. A poll that reaps a completion clears the pace, so a
running transfer is polled back-to-back. Opening a stream restarts it. TX
completions, frame contents, CRC, credits and repost order do not change.

Why: in RX polling the reader re-reads, every ~0.1-0.2 us, the very descriptor
the controller fills next. In interrupt mode an empty read also unmasks the IRQ
(an MMIO read+write), which spaces those reads by ~1 us. Measured 2026-10-23:
the polled modes lose 7-9 % of Gen3 RX bandwidth (one ring 12.11-12.23 Gb/s
interrupt-driven, 11.17-11.21 Gb/s RX-only polled), only on the receiver, with
the reader far below its per-frame CPU budget. The pace tests whether the extra
cache-line traffic on that descriptor is the cost. It is a diagnostic first:
`tools/stream_mem_bench.c` has the same effect in user space
(`USB4_BENCH_EAGAIN_PAUSE_NS`) without a driver change.

Latency cost: a frame that completes during a pace is seen at most
`rx_poll_pace_ns` later. Bandwidth and latency of the pace have not been
measured on hardware yet; both must be measured together, interleaved against
the unpaced polled arm and the interrupt-driven arm.
