# Strix Halo USB4 driver — integrated development series

The maintained result is **all nine patches in `patches/series`**, applied in
order to Linux `72d3fcf802c45d00b300f25b848a93c3a2bd7c7e` (7.3-rc5).
**Raw `overlay/` is an immutable original capture, not the current driver.**
No ZIP or unmerged development branch is needed for the code integrated here.
Start with [build and test instructions](docs/INSTALL-AND-TEST.md).

## Current software

- Shared DMA credit accounting, MSI-X bank handling and batched descriptor
  submission; bounded queues of up to **eight operations per direction**.
- Independent per-slot fences, failed external dependency handling, finite DMA
  deadlines and DMA progress independent of the ioctl mutex.
- Session recovery, statistics, detached-peer quarantine reclamation using the
  original owner and affirmative ring/callback/path-stop proofs.
- Bounded property notifications, safe retirement of quiescent contexts even
  with old FDs open, and RCU-protected concurrent polling.
- Explicit administrative group reset for up to 16 owner-held GPU FDs, plus
  optional cooperative USB4NET close/reset/reopen. Unknown or active consumers
  veto reset. Neither reset nor timeout authorizes unproved memory reclamation.

The matching engine repository is `hyukudan/strix-halo-engine-lab`. Its current
multi-slot entrypoint is **`strix-gpu-queue-visibility`**, built from
`lab/gpu_queue`, with `--queue-depth 2..8` (default 8). The older C1 executable
is retained for compatibility; it does not describe the multi-slot route.
Multi-slot sessions use completed batch windows, not a sliding/adaptive model
scheduler. Production model admission and hardware qualification are not granted
by merging this source.

## Reconstruct and test without Actions

Supply a pristine checkout of the pinned Linux revision. In addition to the
captured files, the builder validates `tb.c`, `tb.h` and USB4NET `main.c` from
that checkout against `provenance/lifecycle-base.json`.

```sh
python3 tools/prepare_overlay.py audited-overlay \
  --kernel-source /absolute/path/to/pristine-pinned-linux
USB4_SOURCE_TREE="$PWD/audited-overlay" \
  USB4_KERNEL_SOURCE=/absolute/path/to/pristine-pinned-linux \
  python3 -m unittest discover -s tests -v
```

`tools/validate_native.py` provides a separate full offline kernel/module and HIP
build plan/executor, including the GPU-on/off matrix. It does not install, boot,
load, sign or execute the binaries. See the installation guide before using it.
The workflows remain unchanged; this integration does not invoke Actions.

The cumulative package passed 81 driver, 101 engine and 12 package-integrity
checks with GCC and Clang. They use explicitly substituted kernel/HIP/MMIO/DMA
boundaries, not physical hardware. The publication also reran all 101 engine
checks with both compilers using the driver's canonical tests rather than copied
headers. Native pinned-kernel/module and ROCm builds have **not** been certified
here. Neither physical GPU visibility nor 40 Gb/s negotiation has been proven.

## Documentation history

The initial audit and intermediate reports describe the state at their own
patch level; their former pending lists are not the current implementation list.
Read these in order where historical details matter:

1. [Initial audit](docs/AUDIT-2026-10-16.md), patches 0002–0004.
2. [Queues and session recovery](docs/QUEUES-AND-RECOVERY.md), patch 0005.
3. [DMA progress and exclusive reset](docs/RECOVERY-2026-10-16.md), patch 0006.
4. [External dependencies](docs/EXTERNAL-DEPENDENCIES-2026-10-16.md), patch 0007.
5. [Detached lifecycle](docs/LIFECYCLE-2026-10-16.md), patch 0008.
6. [Cooperative recovery and retirement](docs/SOFTWARE-COMPLETION-2026-10-16.md), patch 0009.

A sysfs speed of 20 Gb/s per lane with two lanes already means 40 Gb/s raw in
that direction. Application throughput is separate. Use
`python3 tools/usb4_diagnose.py --logs` on both hosts; no physical retraining or
forced speed change is implemented by the recovery sequence.

## Immutable capture and attribution

`overlay/`, `patches/0001-usb4-current-source.patch`, the original source manifest
and reference-patch attribution are preserved. Apply 0001 once, as the first
series entry; reference correspondence is already incorporated and must not be
applied again. The capture's kernel release string does not prove the provenance
of any loaded binary. No signing keys, hardware evidence or compiled binaries
are committed. See [ATTRIBUTIONS.md](ATTRIBUTIONS.md) and [BUILD.md](BUILD.md).
