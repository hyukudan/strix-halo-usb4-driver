# Changelog

## 2026-10-24

Stable series 0001–0048; experimental 0049–0051 in a separate directory.

### Stable (`patches/series`)

- **0047** host RX-only polled completion (`rx_busy_poll`, opt-in): RX without
  interrupts, reaped inline by the reader; TX keeps its interrupts. Measured
  2026-10-23: one Gen3 ring receives 7–9 % less than with interrupt RX, so it
  stays off.
- **0048** host descriptor batching (`rx_repost_batch`, `tx_submit_batch`,
  opt-in, 0..64 frames). Measured 2026-10-23: no bandwidth change (an RX batch
  never fills); stays off.
- The `thunderbolt_stream` module loaded on both lab machines since 2026-10-23
  is the 0001–0048 build (srcversion `3A05C2B1A13ABAEF9B767EF`, reproduced from
  this tree).

### Experimental (`patches/experimental/`, not on hardware yet)

- **0049** bounded RX polling window (poll lease): `rx_window_bytes`,
  `rx_window_us`, BEGIN/END ioctls in the new UAPI `linux/strix_host_stream.h`.
  Changes the core; core and stream modules must be rebuilt together.
- **0050** paced RX descriptor polls (`rx_poll_pace_ns`).
- **0051** TX memory-path experiments (`tx_copy`, `tx_frame_bytes`,
  `tx_no_snoop`). Its hardware measurement plan is under review.

### Tooling and tests

- `tools/apply-series.sh --with-experimental` and
  `tools/prepare_overlay.py --experimental` apply 0049–0051 on top of the stable
  series; without the flag nothing changes.
- CI runs the offline tests and the kernel builds for the stable tree and for
  the experimental tree.
- `provenance/software-validation.json` covers the stable series and pins a
  published engine commit with the same UAPI and queue sources.
- New offline tests for 0047–0051 (host RX-only polling, batching, the 0049
  window with the actual core and stream functions under ASan/UBSan, pacing,
  TX copy modes); `tools/stream_mem_bench.c` gains `USB4_BENCH_EAGAIN_PAUSE_NS`.

### Documentation

- README: status table, current measured figures with dates and conditions
  (per-stream rates and common-window throughput labelled separately), routing
  per message class with the measured host versus GPU-direct comparison, and
  what is still to be measured for each experimental patch.

## Earlier

Series 0001–0046 and the README rewrite: see the README patch table.
0040–0043 add completion-path timing, opt-in GPU polled completion, CPU latency
QoS and a stall detector; 0044 (per-CPU completion work) was measured harmful
and stays off; 0045 adds contiguous DMA buffers; 0046 adds host latency
tracepoints.
