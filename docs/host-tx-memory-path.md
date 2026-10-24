# TX memory-path experiments (experimental, 0051)

Patch 0051 adds three `thunderbolt_stream` module parameters. They apply to
host streams opened afterwards; GPU-direct sessions ignore them. With the
defaults (`tx_copy=0`, `tx_frame_bytes=0`, `tx_no_snoop=N`) the code path is
unchanged. They exist to measure where the per-ring host rate is lost, not as
production settings.

| Parameter | Values | Effect |
|---|---|---|
| `tx_copy` | 0 cached copy (default); 1 non-temporal stores (`_copy_from_iter_flushcache`, then `wmb()`); 2 copy, then write the lines back and keep them (`clwb`); 3 copy, then write back and evict (`clflushopt`) | How `write()` fills a TX frame buffer. Modes 2 and 3 are x86 only (elsewhere they fall back to 0). Every mode sends the same bytes |
| `tx_frame_bytes` | 0 (= 4096) or 256..4096 in steps of 64 | Payload bytes per TX frame. The receiver already accepts any frame size, so only the sender needs it |
| `tx_no_snoop` | N (default) / Y | Sets the PCIe No Snoop option (bit 29) of the stream's TX ring through the new core flag `RING_FLAG_NO_SNOOP`. Requires `tx_copy` 1 or 3, otherwise `open()` fails with EINVAL. The RX ring is never affected |

Why: one host ring moves 9-12 Gb/s on Gen2 and Gen3 alike, the rate drops
when the sender's DRAM or CPUs are loaded (not the receiver's), and the same
ring measures 12.1 Gb/s when its frames are GPU DMA-BUF pages, which the CPU
never wrote: the same as a host ring with contiguous buffers (an earlier
version said ~22.8 Gb/s; that figure was two rings). The per-ring limit therefore looks like bytes-in-flight over the
latency of the controller's TX reads, and a host frame, freshly written by
`copy_from_user`, is still dirty in the CPU caches when the controller reads it.
`tx_copy` changes only that cache state; `tx_frame_bytes` separates a fixed
per-frame cost from a per-byte one; `tx_no_snoop` removes the snoop of the
payload reads altogether.

Risks:

* `tx_copy` 1-3 cost CPU time per frame (non-temporal stores are usually
  cheaper than a cached copy plus flush). Data and ordering are unchanged.
* `tx_no_snoop` is experimental: whether the AMD NHI also reads the
  **descriptors** without snooping is not documented. The descriptors live in
  coherent memory written with ordinary stores; if the controller fetched them
  without snooping it could see stale entries (stale frames, detected by the
  verifying benchmark) or never see the completion (the stream stalls). Use it
  last in a test session, with a verifying benchmark, and expect to reload the
  driver or reboot if a ring stalls. It has no effect if the NHI's PCIe
  `DevCtl.EnNoSnoop` is clear.

Status: built and CPU-tested only; not installed or measured on hardware. The
measurement session (each `tx_copy` mode and frame size against the default,
interleaved A/B/A x3 in both directions on one Gen3 ring, with read-only ring
captures) is under review and has not run. `tx_no_snoop` is excluded from it.
