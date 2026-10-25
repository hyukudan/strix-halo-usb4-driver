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

Status: experimental; measured on hardware on 2026-10-25. The measurement
plan was executed for the copy modes and frame sizes below, except
`tx_no_snoop`, which was not measured (needs the core flag). One host stream,
one Gen3 host ring, machine A → machine B, with 0001–0048 + experimental 0051
loaded only for the measurement window; the stable 0001–0048 module was
restored afterwards. Every arm used A/B/A × 3 against the default (cached
copy, 4096-byte TX frames), with bytes verified. Medians:

| Arm | Send Gb/s | Ratio vs default | Receive ratio |
|---|---|---|---|
| Default (`tx_copy=0`, 4096-byte frames) | 9.18 (18 runs, 9.15–9.33) | 1 | 1 (9.12 Gb/s) |
| `tx_copy` non-temporal | 7.79 | 0.850 | 0.854 |
| `tx_copy` write back (`clwb`) | 9.09 | 0.992 | — |
| `tx_copy` write back and evict (`clflushopt`) | 7.78 | 0.849 | 0.862 |
| `tx_frame_bytes=1024` | 10.00 | 1.085 | — |
| `tx_frame_bytes=2048` | 9.70 | 1.052 | — |
| Default on the Gen2 cable | 8.95 | 0.999 vs its own default | — |
| Default, both rings of one Gen3 cable at once | 20.98 (2 × 10.5) | — | — |

A live register capture during the default send showed the sender's TX ring
always full (p50 1021 of 1024 frames posted, never empty) and the receiver
always supplied (p50 1022 of 1023 RX descriptors posted).

One host Gen3 ring: 9.18 Gb/s send; non-temporal copy / `clflushopt` −15 %
(the controller reads cached payload lines faster than DRAM), `clwb` −0.8 %;
1024-byte TX frames +8.5 %, 2048 +5.2 % on one ring, send only: a candidate,
not a default. The sender's TX ring stays full, so the per-ring limit is the
controller's TX read path, not software or the receiver. The earlier per-ring
gains of contiguous buffers (0045) and GPU-direct rings (~12.1 Gb/s) are
consistent with this (less read latency per frame), but were measured on other
days.

Decisions:

* `tx_copy` 1 and 3 stay off: inherently slower reads from DRAM.
* `tx_copy` 2 stays off (neutral); `tx_copy` 0 stays the default.
* `tx_frame_bytes` 1024/2048 is a candidate, not a default. Still to measure:
  receive direction, four streams at once, the engines' message sizes and
  interaction with `contig_buffers`.
* `tx_no_snoop` is not measured (needs the core flag). 0051 stays experimental.
