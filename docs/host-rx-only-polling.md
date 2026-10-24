# Experimental host RX-only polling

Patch 0047 adds the per-stream configfs boolean `rx_busy_poll` (default 0).
Set it only while the stream is closed and detached from a running session.
It is rejected for a GPU-direct stream; enabling GPU-direct is also rejected
while either polling mode is enabled.

With `rx_busy_poll=1`, RX is allocated without interrupts and the reader reaps
completed RX descriptors inline. NAPI is disabled for this stream. TX retains
its existing interrupt flags, throttling, workqueue and completion batching.
`busy_poll=1` still polls both rings and takes precedence when both are set.
No packet, CRC, descriptor payload or contiguous-buffer policy changes.

During a running session, RX completions and descriptor reposting advance only
when the reader calls read(). If the reader is idle, peer TX can wait for
end-to-end credits until reads resume.

Like full busy polling, RX-only mode returns `EPOLLERR` from `poll()` because
RX has no interrupt to wake a waiter. Use a bounded nonblocking read/write
loop, for example the engine's explicit spin transport mode. Blocking reads
spin with `cond_resched()` and signal checks. No CPU QoS policy changes.

This is a hypothesis for a controlled comparison, not a recommended default.
Full polling improved after-idle small-message latency but reduced concurrent
Gen3 per-ring bandwidth relative to interrupt-driven C3 in the lab. RX-only
isolates completion policy by preserving TX; it does not establish that TX
caused the entire bandwidth difference. Compare IRQ+C3, RX-only+C3 and full
polling+C3 with identical pinning, payloads, verification and descriptor traces.

Offline host-path fixtures exercise all four full/RX-only combinations,
TX interrupt preservation, RX reaping, partial nonblocking reads, TX credits,
CRC failure, poll rejection and teardown. Hardware timing and model gates
remain mandatory before adoption. Installing the new kernel/modules belongs
to the operator's reviewed maintenance window.
