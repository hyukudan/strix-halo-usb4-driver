# Host completion latency tracing

Patch 0046 adds `thunderbolt:tb_latency`, disabled until explicitly enabled in
tracefs. It does not change queue choice, affinity, interrupt moderation, buffer
allocation, completion ownership or payload bytes. Keep 0044 off unless an
isolated experiment requests it; 0045 remains independent.

Use a private trace instance with the `mono` clock and a short bounded capture.
Record the IRQ, workqueue and scheduler events together with this event. Each
record identifies the ring, NHI, frame where available, direction and hop. Time,
CPU and TID are provided by the trace header. Numeric stages are:

| stage | observation | value |
| --- | --- | --- |
| 1 | IRQ dispatch before NAPI/workqueue selection | 0 |
| 2 | ring worker entered, before taking ring lock | 0 |
| 3 | completed frames collected under ring lock | tail |
| 4 | worker callbacks finished | 0 |
| 5 | NAPI reap entered with a live RX ring | spin requested |
| 6 | NAPI collected a frame, before callback | frame size |
| 7 | stream waitqueue wake about to be issued | poll event mask |
| 8 | descriptor producer/consumer doorbell published | head |
| 9 | RX completion published by callback | frame size |
| 10 | TX credit returned by callback | 0 |

Stage 3 also covers the polled collector; stage 6 plus stage 9 proves that a
host NAPI reader actually collected and delivered a completion. Stage 7 records
the NAPI direct wake and normal TX wake. Normal RX callback wake follows stage
9; use scheduler wakeup events to identify a task actually made runnable.

These are software observations. They do not measure the physical arrival time
on the cable, expose a hardware E2E credit counter or synchronize clocks between
hosts. Correlate per exchange using local monotonic CPU timestamps from the
application, plus ring/work identity and scheduler TIDs. Preserve requested and
effective IRQ affinities, per-thread masks, tracing overruns and binary hashes.
An attribute accepting `irq_cpu` is insufficient proof of effective affinity.

Tracing adds overhead when enabled. Capture attribution separately from the
untraced latency and bandwidth runs; do not combine their percentiles. The
administrative helper in `tools/lab/usb4lat2-root.py` uses fixed arms, private
tracefs state, at most five seconds per capture, and restores the saved stream
attributes and module parameters on exit. It requires administrative review and
startup; application processes continue to run as the ordinary lab user.

The revised helper allocates 8 MiB per CPU (about 256 MiB on a 32-CPU host),
keeps overwrite disabled and saves per-CPU drop counters for every capture.
Attribution runs must be short enough to avoid drops. It disables the event
hooks and selects the `nop` tracer between captures, after saving trace data
and counters, so plain sessions do not retain enabled tracing hooks.

Fixed arms `BP` and `BP3` select existing `busy_poll=1`, `rx_napi=0`, unbound
completion work and contiguous buffer modes 0 and 3 respectively. No new
kernel policy is introduced. Busy-poll rejects `poll(2)`; applications must
explicitly select bounded nonblocking read/write loops, including pairing.
These arms are diagnostic only until byte, model, power and bandwidth gates
pass. No arm changes CPU latency QoS.
