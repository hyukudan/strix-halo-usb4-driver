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
untraced latency and bandwidth runs; do not combine their percentiles. Keep
captures short (a few seconds), use a private tracefs instance with overwrite
disabled and a buffer large enough to avoid drops (8 MiB per CPU was enough for
a 20 KB exchange loop), save the per-CPU drop counters with every capture, and
disable the event and select the `nop` tracer between captures so untraced runs
do not keep the hooks enabled. Restore any stream attributes or module
parameters changed for an experiment when it ends.

Busy-poll streams (`busy_poll=1`) reject `poll(2)`: applications must use
bounded non-blocking read/write loops, including for pairing.
