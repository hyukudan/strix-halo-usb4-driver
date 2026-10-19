# Per-stream throughput ceiling (2026-10-19)

## Finding

One USB4STREAM stream moves about 9.3 Gb/s whatever the link generation.

- That is 3.5-3.7 us per 4 KiB frame, and it holds inside a single message too.
- Two streams of one cable reach twice that (18.7-18.9 Gb/s); four streams over two cables reached
  35-40 Gb/s.
- The ceiling is per DMA ring of the AMD host interface. It is on the transmit side: DRAM load on the
  sender lowers it (9.2 -> 8.3 Gb/s), while DRAM load on the receiver, CPU load, CPU frequency, interrupt
  moderation, `dma_credits` and the PCIe MRRS do not change it.
- Each NHI has three rings (`REG_CAPS`), so a cable carries at most two streams.
- AMD has a firmware change that raises single-ring USB4 point-to-point throughput from about 9 to about
  30 Gb/s on the same SoC. BIOS 3.11 of these boards does not have it.

## 0020

The `e2e` module parameter (default true) and the per-endpoint configfs attribute `e2e` (-1 follows the
parameter, 0 off, 1 on) choose whether a stream's rings use end-to-end flow control when it is opened.

- The default behaviour is unchanged.
- The setting exists only to measure what E2E costs.
- Without E2E, frames that find no posted RX descriptor are dropped, and the stream does not retransmit.
- Both ends must use the same value.

## Pending hardware checks

1. `ring_size` 64/256/1024/4096: does a fresher TX buffer, still in cache, change the rate?
2. `e2e=0` on both ends of one pair, with the verifying benchmark.
3. IOMMU identity domain for the NHIs: the per-frame translation cost.
4. A BIOS with the AMD change.
