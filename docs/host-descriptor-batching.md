# Experimental host descriptor batching

Patch 0048 adds two independent per-stream configfs budgets:
`rx_repost_batch` and `tx_submit_batch`, each 0..64 frames, default 0.
Zero retains individual submissions. Values can change only while closed;
nonzero budgets and GPU-direct are mutually rejected, including at GPU open.

RX batches only buffers already consumed by this read. Each buffer is reset
and synchronized for DMA before it enters the local list. The list is
published at its budget and at every exit from the copy loop, including
partial copies, faults and CLOSE. It never waits for another frame to fill a
batch. An enqueue failure detaches every local list node, preserves bytes
already delivered and latches EIO for subsequent reads.

TX copies and reserves at most the budget or the currently available credits,
then submits them with the existing atomic NHI list API. A later copy fault
still submits the complete prefix. If submission fails, only this unsubmitted
batch is rolled back: iterator bytes, reserved credits and list ownership.
Previously submitted batches remain the positive return value. CLOSE keeps
its existing individual submission and bounded drain.

No wire bytes, frame boundaries, CRC, floating-point work, interrupt flags,
moderation, callback/wake counts, contiguous buffers or QoS policy change.
The existing NHI list API takes its ring lock once and writes the descriptor
doorbell once after adding the available descriptors. Completion callbacks
may run immediately after submission; bookkeeping must precede publication.

Measured 2026-10-23 (both nodes, 4 host streams over both cables, every byte
verified): an RX repost budget of 64, alone or with a TX budget of 64, left the
bulk rate unchanged (sum of the four per-stream rates 42.62 Gb/s with
interrupt-driven RX and `contig_buffers=3`, 40.13-40.21 Gb/s with RX-only
polling). The cause is in the code, not the hardware: `read()` publishes its
repost list before it returns and each call consumes about 1.16 frames on
average, so an RX batch never fills; TX batching does group frames on the
sender, but the sender is not the bottleneck. The budgets stay available,
default 0, with no performance claim.
