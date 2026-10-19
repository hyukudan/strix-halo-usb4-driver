# Review fixes 0012–0016 (2026-10-19)

State: implemented and tested **offline only** (unit tests with sanitizers and
`W=1` object builds against pinned 7.3-rc5). Nothing here has run on the
Strix Halo nodes yet. The hardware checks at the end must pass before this
series becomes the default kernel.

Source of the findings: the deep review of branch
`feat/per-stream-latency-controls` (3d11d18) and the cluster1 incident of
2026-10-19 (streams of NHI1 failing with `ENOMEM` after one failed open).

## 0012 — `tb_stop()` unlinks the tunnels it frees (review C-2)

**Bug.** `tb_stop()` dropped every tunnel with `tb_tunnel_put()` but left it
on `tcm->tunnel_list`. 0009 added a later reader of that list: domain
retirement (`tb_domain_remove()` → lifecycle notifier → `tbstream_gpu_stop()` →
`tb_domain_dma_paths_busy()` → `tb_dma_paths_busy()`). Any reboot, shutdown or
NHI unbind with an armed `gpu_direct` session walked freed tunnels: an oops, or
a garbage "no DMA path" that let the driver free memory still mapped for DMA.

**Fix.** `list_del()` before `tb_tunnel_put()` in `tb_stop()`. After stop the
inventory is empty, which is the correct answer (DMA tunnels were deactivated,
the others are no longer tracked). `tb_dma_paths_busy()` also refuses a
non-empty inventory once the root switch is gone (`-ENODEV`, no proof) instead
of walking it.

**Tests.** `tests/test_tb_stop_inventory.py` compiles the actual `tb_stop()` of
the patched `tb.c` with heap-allocated tunnels under AddressSanitizer, then runs
the actual query. It fails on the unfixed tree (stale list) and passes now.
`dma_path_query_tests.c` covers the new guard.

## 0013 — No hang on reboot/unbind with a retained GPU context (review H-0, M-11)

**Bug.** A GPU context that cannot prove its DMA stopped is quarantined and
keeps a reference on the domain (by design: "never fake DMA completion"). The
PCI remove/shutdown path then waited for the domain with a non-interruptible,
unbounded `wait_for_completion()`: reboot never finished. After a configfs
`rmdir` the `gpu_recover` attribute disappeared, so such a context could not be
reaped any more either.

**Fix, in three layers.**

1. *Hardware proof at teardown.* Consumers now report each retained owner to
   the core (`tb_domain_dma_retained()`). If any remain after the existing
   `TB_DMA_DOMAIN_QUIESCING` pass, the core clears the NHI's PCI Bus Master
   Enable, reads `PCI_COMMAND` back to verify it, and reads an NHI register
   (non-posted reads cannot overtake earlier posted DMA writes). A function
   that does not answer is no proof (the series rule), it is abandoned. Otherwise it sends
   `TB_DMA_DOMAIN_DMA_REVOKED`. The GPU extension stops and joins the rings,
   fails the fences that can never complete with `-ENODEV` and releases the
   context. This proof is stronger than the ring/callback/path proofs used
   elsewhere, and it is only used at the point where the controller is going
   away anyway.
2. *Leak instead of use-after-free.* If no proof is possible (non-PCI NHI,
   bus-master bit stuck, a ring whose software state still runs), the core
   sends `TB_DMA_DOMAIN_ABANDONED`. The context is marked abandoned: it keeps
   every DMA resource and reference forever (memory stays pinned until reboot),
   every ioctl returns `ENODEV`, close keeps it, and it never touches the
   controller again.
3. *Bounded wait.* `tb_domain_wait_released()` replaces the unbounded wait:
   10 s, a warning, up to 30 s in total (immediately if owners were
   abandoned). On timeout the domain is detached from the NHI under a lock
   shared with the domain release: its control channel is freed while the NHI
   still exists, and `tb->nhi`/`tb->ctl` become `NULL`, so a late release
   touches neither. `nhi_shutdown()` reports leaked rings of an abandoned
   domain with `dev_err` instead of `dev_WARN`.

**Trade-off.** Freeing memory that a controller might still write corrupts
whatever reuses it; leaking it costs memory until reboot. The series keeps the
leak. A reboot or unbind now always completes; at worst it logs that a domain
or GPU memory was abandoned.

**rmdir.** Removing the configfs item first tries to reap a quarantined
context (no open FD). If it stays quarantined, the kernel logs it and
`echo 1 > /sys/module/thunderbolt_stream/parameters/gpu_reap_orphans`
(CAP_SYS_ADMIN) retries every orphan with the same proofs as `gpu_recover reap`
(never a reset). NHI removal also releases them through the bus-master proof.

**Tests.** `test_dma_recovery` (`teardown_revoke`) runs the real core sequence
against PCI doubles: nothing retained, released after revoke, still retained
(abandoned), bus-master bit stuck, config read failure, non-PCI, device gone.
`test_gpu_queue` adds `domain_revoked_releases_retained`,
`domain_abandoned_leaks_safely` and `orphan_reaping_after_rmdir`.

**Not covered offline.** `tb_domain_wait_released()` and the PCIe ordering
argument. The AMD NHI is an integrated function: that clearing Bus Master
Enable really stops its DMA (and that its completions are not reordered by
Relaxed Ordering) must be confirmed on hardware.

## 0014 — A failed open no longer leaves the NHI unusable (cluster1 incident)

**Bug.** `tbstream_dev_start()` enables the DMA paths, starts the rings, then
allocates the RX buffers. If that last step failed (the cluster1 order-4
allocation failure), the error path stopped and freed the rings but never
disabled the paths: the DMA tunnel stayed in the connection manager, holding
the NHI ring hops and the stream's HopIDs. Every later open of **either**
stream of that NHI got the same ring hop and `tb_tunnel_alloc_dma()` could not
reserve it, so `tb_approve_xdomain_paths()` returned `-ENOMEM`, silently, until
the module was reloaded. The same path also leaked the partially allocated
pages and DMA mappings, and left dangling ring pointers.

**Fix.** The error path disables the paths before freeing anything, frees
partial TX/RX buffers, clears the ring pointers, restores IRQ affinity and logs
`tbstreamN: open failed: <errno>` (ratelimited). The frame bookkeeping arrays
(up to 4096 × ~64 bytes, never DMA targets) use `kvcalloc`, so an open no longer
needs an order-4 contiguous allocation. Descriptor rings stay
`dma_alloc_coherent` (the DMA API needs them contiguous; 16 KiB at 1024
entries).

**Tests.** `tests/test_stream_host_path.py` compiles the actual host-path
functions of `stream.c` against a model of rings, DMA mappings, IRQs and the
CM tunnel inventory. `open_failure_is_not_sticky` injects a failure at each of
nine points; after each one nothing must remain and both streams of the NHI
must open again. With the path-disable line removed it fails exactly like
cluster1 ("rx frame array failure left tunnels=1").

**Other cluster1 symptom (NHI0, `DMA RX path activation failed: -110`).** Not
fixed here; what the code shows:

- `-110` is `-ETIMEDOUT` from the control channel (`ctl.c`: "timeout reading
  config space") or from `__tb_path_deactivate_hop()`, which gives up after
  500 ms when a hop's `pending` bit never clears ("hop deactivation failed").
- When activation fails, `tb_tunnel_activate()` deactivates and the tunnel and
  its software HopIDs are freed, but nothing resets a router hop that did not
  drain. The stream's HopIDs are fixed by configfs, so every retry programs the
  same stuck hop entries and fails the same way.
- Only a host router reset clears that state: reloading `thunderbolt` with
  `host_reset=Y` (HRR at probe) or a reboot. The explicit DMA interface reset
  (`gpu_recover reset`, `SGU_IOC_RESET[_GROUP]`) resets the NHI DMA engine, not
  the router hop tables, so it is not expected to help. 0001 removed the
  automatic AMD teardown reset (review H-0b), which is the class of hang its
  quirk described.
- To confirm on the node: whether the timed-out config reads target route 0
  (the host router) and which hop index fails, with
  `echo 'file path.c +p; file ctl.c +p' > /sys/kernel/debug/dynamic_debug/control`.

## 0015 — 0011 follow-up: spin without the device mutex, IRQ affinity restored

**Bug.** With `rx_napi`, the reader spun up to `spin_us` (≤ 1000 µs) inside
`read()` and `poll()` with `sdev->lock` held, so `write()` and `poll()` of the
same stream waited for it. `irq_cpu` changed the affinity of MSI-X vectors that
the next ring (USB4NET, another stream) inherits, and failed silently.

**Fix.** A separate `rx_lock` serializes NAPI reaping and the RX ring teardown.
`read()` reaps once under the device mutex (non-blocking `trylock` of
`rx_lock`), and a blocking reader spins in `tbstream_dev_napi_spin()` with only
`rx_lock` held, then re-takes the device mutex to copy data. `poll()` never
waits for a spinner. `tbstream_dev_stop()` takes `rx_lock` around the ring
teardown. The affinity in force before `irq_cpu` is saved per vector and
restored before the rings are freed (stream stop, failed open, GPU context
close). An offline `irq_cpu` or a refused change is logged. irqbalance can still
move a pinned vector; ban those IRQs there if the placement must hold.

**Tests.** `spin_without_device_mutex` parks a spinning reader inside
`tb_ring_poll()` and checks that another thread takes the device mutex at once;
`spin_is_bounded_and_rearms`; `irq_affinity_is_restored`.

## 0016 — Kernel-validated terminal frame for `gpu_direct` (review H-2)

**Problem.** RX frames land in descriptor posting order and the kernel only
checks each frame's length. If the peers disagree on `payload_bytes` or on
SUBMIT order and the sizes happen to match, data shifts silently.

**Fix (opt-in, per operation).** `SGU_PREPARE_TERMINAL_HEADER` in
`sgu_prepare.flags` (the former `reserved` field, kept as a union member so old
sources still compile). `terminal_bytes` must then be 64. TX sends a header the
kernel builds from the operation; RX receives the terminal into a kernel page
(a full page, so a misrouted 4 KiB payload frame stays in kernel memory) and
checks length, magic, version, the import's `connection_epoch`, `sequence`,
`payload_bytes`, payload frame count, slot and reserved fields. A mismatch sets
`SGU_FRAME_ERROR_TERMINAL` (128) on the terminal frame, EVENT reports
`SGU_EVENT_FAILED`, the session fails and the kernel logs expected and received
values. Contract: both peers set the flag on matching operations, and an RX
operation uses the same sequence as the peer's TX operation it receives.
Standby RX reuse cannot switch the mode of already posted slots (`EOPNOTSUPP`).

**What it does not do.** It validates framing, not payload integrity: with
only the link CRC, an end-to-end checksum of the payload is still the
application's job. In header mode the terminal no longer lands in the GPU
window, so a GPU kernel cannot poll it (review §3.4a); that would need a
separate design.

**Versioning.** `abi_minor` is part of every identity (`struct sgu_fence`) and
QUERY requires an exact match, so raising it would break every existing client.
It stays 0. Additive extensions are advertised as capability bits in
`SGU_IOC_QUEUE` (`SGU_CAP_TERMINAL_HEADER` = 256); older kernels reject a
non-zero `flags` with `EINVAL`. Struct sizes, offsets and ioctl numbers are
unchanged (`test_gpu_abi`). A future minor bump should come together with a
QUERY that accepts `minor <= current`.

**Tests.** `terminal_header` (TX header contents, RX accepted, RX shifted by one
operation rejected), `terminal_mismatches` (each field, a payload frame in the
terminal slot, little-endian wire format), `terminal_standby_reuse`.

## Synchronization contract with HIP/KFD (review C-1, documented only)

The driver waits, on the CPU and for at most 1 s per attempt, only for fences
in the DMA-BUF's `dma_resv`: TX waits for `WRITE` fences, RX for `READ` fences.
HIP kernels run on KFD user queues, which publish no fence there (KFD BOs carry
only an eviction fence with `BOOKKEEP` usage, which the driver correctly
ignores). Therefore, with HIP/ROCm memory:

- before `SGU_IOC_SUBMIT` (TX) the application must have completed the GPU work
  that writes the window (`hipStreamSynchronize`, `hipEventSynchronize`, or a
  completion flag it checks on the CPU);
- before `SGU_IOC_RX_REPOST`, or `PREPARE`/`PREPARE_SLOT` reusing an RX slot, it
  must have completed every GPU kernel that reads that slot;
- the controller fence (EVENT, sync_file) means local DMA completion only: not
  GPU visibility and not a remote acknowledgement.

Violating this corrupts data silently. Whether an explicit in-fence
(`sync_file`) on SUBMIT should be added, and how a HIP stream could wait on the
RX side, are open design decisions that need hardware confirmation.

## Hardware validation before installing the kernel

Run on both nodes, one at a time, with the current kernel kept as the default
boot entry. Prefer a build with `CONFIG_KASAN=y`, `CONFIG_PROVE_LOCKING=y` and
`CONFIG_FAIL_PAGE_ALLOC=y` for the first pass; then the production config.

1. **Build and boot (not default).** Apply `patches/series` (0001–0016) plus
   `patches/kernel-backports/`, build with a distinct `LOCALVERSION`, boot it
   once via a one-time boot entry. Check `dmesg` is free of warnings, KASAN and
   lockdep reports after the links come up (Gen3 × 2).
2. **Regression.** Repeat the README campaigns: 4-stream bandwidth with
   integrity, `pingpong` latency (interrupts and `rx_napi=1 spin_us=25
   irq_cpu=<reader CCD>`), 80 open/close cycles, GPU one-way/duplex, recovery
   and group reset. Expect the 2026-10-17/18 numbers.
3. **0014, sticky ENOMEM.** With `fail_page_alloc` restricted to one process
   (`task-filter=Y`, `times=1`, `probability=100`, `interval=1`, `min-order=0`,
   `ignore-gfp-wait=N`, then `echo 1 > /proc/self/make-it-fail` in the shell
   that opens), open a stream: it must fail with `ENOMEM` and log
   `tbstreamN: open failed: -12`. Without the fault, both streams of that NHI
   must then open and pass data, with no module reload. Also check that
   `/proc/interrupts` has no leftover ring vectors and the open/close counts
   in `dmesg` match.
4. **0015, spin.** On one stream set `rx_napi=1 spin_us=1000 irq_cpu=N`.
   Full duplex on the same FD pair (writer and reader threads): the writer's
   per-`write()` latency must not show 1 ms steps; compare with the 0011
   module. `cat /proc/irq/<tx,rx>/smp_affinity_list` before open, while open
   (= N) and after close (restored). Check USB4NET (if loaded) still gets its
   own placement after a stream closes.
5. **0012/0013, teardown with an armed session.** Start a `gpu_direct` session
   (engine `strix-gpu-queue-visibility`) and, while it is armed, unbind the NHI:
   `echo <bdf> > /sys/bus/pci/drivers/thunderbolt/unbind`. It must return within
   30 s, with no oops/KASAN report; GPU memory must stay valid for the
   application. Rebind and repeat with `systemctl reboot` (it must complete).
   Look for `bus mastering revoked` or `abandoning` lines: the first is
   expected only if a context was retained, the second should not appear on
   these PCI NHIs.
6. **0013, rmdir.** With a stream closed, `rmdir` its configfs item, recreate
   it, and check `echo 1 > /sys/module/thunderbolt_stream/parameters/gpu_reap_orphans`
   returns success.
7. **0016, terminal header.** Needs a client that sets
   `SGU_PREPARE_TERMINAL_HEADER` with `terminal_bytes=64` on both peers (the
   engine runner does not yet). Run the duplex test with it, then once with
   the RX sequence deliberately off by one: EVENT must report
   `SGU_EVENT_FAILED` with frame error 128 and `dmesg` the mismatch.
8. **NHI0 of cluster1.** Before any of the above, recover it (reload
   `thunderbolt` with `host_reset=Y`, or reboot) and capture the dynamic-debug
   output described in 0014 if `-110` returns.
