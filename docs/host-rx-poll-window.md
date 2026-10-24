# Bounded host RX polling windows (experimental)

Patch 0049 adds an opt-in window on IRQ-allocated RX/NAPI rings. The default
`rx_window_bytes=0` leaves the existing path enabled. TX interrupt mode, frame
contents, CRC, batching budgets and CPU power policy do not change.

Configure only a closed host stream: `rx_window_bytes` is 0..1048576 and
`rx_window_us` is 1..10000 (default 1000). A nonzero threshold excludes static
busy polling and GPU-direct (EOPNOTSUPP) and additional opens (EBUSY). It is also
mutually exclusive with the 0048 descriptor batching budgets: a nonzero
`rx_window_bytes` store returns EBUSY while `rx_repost_batch` or
`tx_submit_batch` is nonzero, and a nonzero batch store returns EBUSY while
`rx_window_bytes` is nonzero. Storing 0 is always accepted, so clear one side
before enabling the other. It enables RX NAPI even if the module-wide NAPI
setting is off. Ring flags retain normal IRQ allocation.

The project-owned `strix_host_stream.h` ABI defines BEGIN and END with a fixed
24-byte cookie/bytes/flags/reserved record. Use a nonblocking host fd, a nonzero
unique cookie and the full logical exchange size, including its trailer/header.
Do not infer that size from a frame or read syscall. Reserved fields are zero.
BEGIN returns EOPNOTSUPP when disabled, E2BIG above the configured threshold,
EBUSY for an active window and ENODEV for an unavailable endpoint. In these first
two cases the caller can use ordinary IRQ transport. END requires the matching
cookie; a stale cookie returns ESTALE. Matching END is idempotent after expiry.

BEGIN masks only the RX ring interrupt. Existing inline NAPI reaping owns the
same descriptors/callbacks; poll_complete cannot unmask the leased ring. poll()
advertises read readiness during the window so a nonblocking reader keeps
reaping. END or the monotonic hard hrtimer clears the lease and unmasks the IRQ.
This bounds the hardware mask duration, subject to normal interrupt latency;
it is not a hard realtime userspace scheduling guarantee.

The stream mutex serializes BEGIN/END/open/stop. A separate spinlock protects
timer state and the RX ring lifetime; the order is stream mutex, optional RX
mutex, window spinlock, NHI lock, ring lock. The IRQ callback never takes the
window lock. The timer takes neither stream nor RX mutex. Stop/last close
synchronously cancel the timer outside its spinlock before any ring free.
Core stop also clears its lease, and core END avoids MMIO after stop/removal.

Both core and stream require a matching rebuild because the core exports new
APIs and tb_ring layout changes. This is not a stream-only reload onto an older
core. No native kernel build, installation or hardware qualification is supplied
by the CPU-only validation of this patch.

`tests/test_stream_hybrid.py` compiles the actual core lease functions and host
stream functions with ASAN/UBSAN. MMIO, DMA, IRQ delivery and timer scheduling
are doubles. Existing host regression cases also use this harness for 0049.
Checks cover threshold/off/oversize, stale/nested cookies, early/late expiry,
rearm, pending completion ownership, endpoint teardown and END/expiry races.
These tests do not establish DMA coherency, real interrupt delivery or speed.
