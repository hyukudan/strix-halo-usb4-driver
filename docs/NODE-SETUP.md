# Node setup (permanent configuration)

The normal, permanent setup of each of the two nodes (node A and node B: Ryzen AI Max+ 395, linked by two
USB4 cables) and the check to run after every boot. Build details are in
[INSTALL-AND-TEST.md](INSTALL-AND-TEST.md) section 1b; this page says what ends up on each node and how to
verify it. Changing kernels, GRUB, modprobe and systemd files needs root; do it from a console or SSH session
you can recover, with a known-good stock kernel installed.

## 1. Overview

Each node runs the same software:

| Part | What |
|---|---|
| Kernel | Linux 7.3-rc5 with the USB4 series 0001–0047 or 0001–0048 (section 3) and the upstream backport; optionally also the queued AMD IOMMU `amd/amd-vi` series (the *PerfOpt kernel*). A kernel without the IOMMU series is the *plain kernel* below |
| Stream module | `thunderbolt_stream` with patch 0048 (the current variant), in the module tree of every kernel the node boots |
| Lab service | `usb4stream-lab.service`: loads the stream module, creates the configfs streams, writes the layout file |
| Layout file | `/run/usb4stream-layout.json`, written by `usb4stream-describe` at boot (and after hotplug with the optional udev rule) |
| System settings | `kho=off`, TTM `pages_limit`, the `thunderbolt_net` modprobe rule (section 4) |
| NPU runtime (optional) | XRT userspace for the on-die XDNA NPU (section 7); nothing in the driver or the engine requires it |

Run the same kernel kind and the same stream module on both nodes.

## 2. Kernel build

**Base:** Linux `72d3fcf802c45d00b300f25b848a93c3a2bd7c7e` (7.3-rc5), as pinned in the
README. Apply with [`tools/apply-series.sh`](../tools/apply-series.sh)
(`tools/apply-series.sh --with-backports <tree>`); do not add `--with-experimental`.

### USB4 series ([`patches/series`](../patches/series))

The patches have no mail headers; the purpose of each is from the README patch table.

| Patch | Purpose |
|---|---|
| 0001 usb4-current-source | Baseline: configfs USB4STREAM streams, GPU DMA-BUF UAPI, ring framing, DMA credits, NHI fixes |
| 0002 credit-accounting-and-link-diagnostics | Validated `dma_credits`, shared RX credit budget, MSI-X clear index fix |
| 0003 batch-gpu-ring-submission | One barrier and one doorbell per batch of GPU descriptors |
| 0004 fence-and-stop-lifetime | Fence and ring-stop lifetime guards |
| 0005 gpu-queues-fences-and-session-recovery | Bounded GPU queues, `sync_file` fences, fail-closed session recovery |
| 0006 dma-progress-idle-reset-and-quarantine-reaping | Finite DMA deadlines, exclusive idle reset, quarantine until stop is proven |
| 0007 external-dependency-errors | Failed external GPU dependencies are rejected |
| 0008 detached-quarantine-and-notification-lifecycle | Detached-peer quarantine reclamation, bounded XDomain notifications |
| 0009 domain-retirement-and-cooperative-controller-recovery | Context retirement, group reset, optional USB4NET close/reset/reopen |
| 0010 stream-rx-napi-hybrid-completion | Opt-in NAPI-style RX completion (`rx_napi`) |
| 0011 stream-per-stream-napi-and-irq-steering | Per-stream `rx_napi`, `spin_us`, `irq_cpu` |
| 0012 cm-unlink-tunnels-on-stop | `tb_stop()` unlinks the tunnels it frees |
| 0013 bounded-domain-release-and-dma-revocation | Bus-master revocation or abandonment; bounded domain wait |
| 0014 stream-open-error-path-and-frame-arrays | Failed open no longer leaks the tunnel; `kvcalloc` frame arrays |
| 0015 stream-spin-outside-device-mutex-and-irq-restore | `spin_us` polling without the device mutex; IRQ affinity restored |
| 0016 gpu-kernel-validated-terminal-frame | Opt-in kernel-validated terminal header |
| 0017 stream-detach-ram-rings-and-domain-xdomain-teardown | RAM-ring streams stop on service removal; ordered domain teardown |
| 0018 amd-teardown-interface-reset-when-idle | Reset an idle AMD host interface after DMA path teardown |
| 0019 stream-open-bounded-wait-for-peer | `open()` waits at most `open_timeout_ms` for the peer |
| 0020 nhi-shutdown-releases-ring-interrupts | `nhi_shutdown()` frees vectors of rings still registered |
| 0021 stream-device-state-under-lock | Stream validation only under the device mutex |
| 0022 stream-resume-failure-fails-open-files | Failed resume fails the open files |
| 0023 stream-rx-counters-before-posting | RX counters reset before buffers are posted |
| 0024 dma-revoke-proves-relaxed-ordering-off | Revocation proof also requires Relaxed Ordering off |
| 0025 stream-damaged-rx-frame-fails-read | CRC error or overrun makes `read()` return `EIO` |
| 0026 stream-close-bounded-wait-for-tx-slot | Close waits up to 500 ms for a TX slot for CLOSE |
| 0027 stream-partial-io-accounting | Partial and zero-length I/O accounting |
| 0028 stream-close-does-not-flush-rx | Close does not wait for RX buffers that never complete |
| 0029 amd-teardown-reset-pending-until-idle | 0018 reset stays pending until the NHI is free |
| 0030 stream-spin-us-parameter-bounded | `spin_us` bounded to 1000 µs |
| 0031 gpu-nonblock-submit-does-not-sleep-on-mutex | `O_NONBLOCK` submit returns `EAGAIN` instead of sleeping on the mutex |
| 0032 kernel-doc-stream-tbnet-cm-ops-members | Kernel-doc only |
| 0033 xdomain-lane-bonding-result-checked | Lane bonding result checked, at most two retries |
| 0034 host-router-reset-result-checked | `tb_start()` checks the host router reset |
| 0035 opt-in-gen3-retrain | Opt-in (`gen3_retrain`) port reset to bring a Gen2 XDomain link to Gen3 |
| 0036 gpu-nonblock-submit-does-not-sleep-on-dma-lock | `O_NONBLOCK` submit only tries the DMA lock |
| 0037 stream-damaged-close-is-an-error | Damaged CLOSE frame gives `EIO` |
| 0038 teardown-reset-pending-survives-requests-contention-suspend | Pending reset survives contention and suspend |
| 0039 gen3-retrain-serialized-and-exclusive | Gen3 retrain under the domain lock, vetoed by any tunnel |
| 0040 completion-path-timing | Diagnostic: completion timing in configfs `gpu_timing` |
| 0041 gpu-opt-in-polled-completion | Opt-in polled GPU completion (`gpu_poll`) |
| 0042 stream-opt-in-cpu-latency-qos | Opt-in CPU latency QoS while rings run |
| 0043 gpu-tx-progress-stall-detector | Diagnostic: GPU stall episodes (`gpu_stall_ms`) |
| 0044 nhi-opt-in-per-cpu-highpri-completion-work | Opt-in, measured harmful: keep `dma_wq_percpu` off |
| 0045 stream-opt-in-contiguous-dma-buffers | Opt-in contiguous host frame buffers (`contig_buffers`) |
| 0046 host-completion-latency-tracepoints | Diagnostic: `tb_latency` tracepoint |
| 0047 host-rx-only-polled-completion | Opt-in RX-only polling (`rx_busy_poll`), off |
| **0048 host-opt-in-descriptor-batching** | **Opt-in RX/TX descriptor batching (`rx_repost_batch`, `tx_submit_batch`), off. Changes only the stream module: see section 3** |

**Backport** ([`patches/kernel-backports/`](../patches/kernel-backports/)): upstream `e6895021a328`
*drm/amdkfd: fix dma_buf reference leak in get_dmabuf_info*, applied after the series.

### AMD IOMMU series (PerfOpt kernel only)

The IOMMU tree's `amd/amd-vi` branch queues 14 commits for Linux 7.4; the 13 code commits below are applied in
this order on top of the USB4 series (the MAINTAINERS commit is not taken; the USB4 series is unchanged). In our build all 13 applied cleanly with `git apply --check`.

| # | Upstream | Subject |
|---|---|---|
| 1 | `f5ec5d9476c4` | iommu/amd: Do not request ACS when IOMMU is not going to be initialized |
| 2 | `f6e14328620e` | iommu/amd: Disallow implicit START_STATE => IVRS_DETECTED transition |
| 3 | `567e74ce4dfd` | iommu/amd: Remove ad-hoc checks that are never true |
| 4 | `bf5bdfd1f8d4` | iommu/amd: Force identity mode for selected GPUs only (**identity forced only for the APU GPU**) |
| 5 | `15d5b3027d9f` | iommu/amd: Refactor device probe and capability initialization |
| 6 | `5b33c19a31c0` | iommu/amd: Remove iommu_ignore_device() |
| 7 | `622384f54a84` | iommu/amd: Fail probe on ATS configuration failure |
| 8 | `f4eab1f88f79` | iommu/amd: Move GA log allocation to init |
| 9 | `108657f24706` | iommu/amd: Fix leak of data->entry in irq_remapping_alloc() error path |
| 10 | `6e87850d1a8f` | iommu/amd: Add PerfOpt IOMMU performance optimization support (**PerfOpt**) |
| 11 | `e74db71a3530` | drm/amdgpu: Enable PerfOpt IOMMU perf optimization when GPU in identity domain (**PerfOpt**) |
| 12 | `05d62be400f3` | iommu/amd: Program device table when kdump reuse fails |
| 13 | `bae0d3760661` | iommu/amd: Don't allocate kdump device table from DMA32 |

Commit 4 forces identity only for the APU GPU; other devices that were identity before can become translated (on
our nodes, one device did).

### Configuration and toolchain

- `.config` = the running kernel's configuration (`/boot/config-$(uname -r)`), plus
  [`config/usb4-gpu.fragment`](../config/usb4-gpu.fragment) for the first USB4 build. The only other change is a
  new, distinct `LOCALVERSION` (`<LOCALVERSION>`) per build; after `make olddefconfig` the diff against the running
  configuration should be that one line. `tools/check_kconfig.py` rejects a reused or placeholder identity.
- Build with the same compiler and binutils as the running kernel (compare `/proc/version`). Our builds:
  GCC 15.2.0 (Ubuntu 15.2.0-16ubuntu1), binutils 2.46.
- Install next to the previous kernel, never over it (INSTALL-AND-TEST.md section 1b).

## 3. The USB4 stream module

The setup uses the **0048 variant** of `thunderbolt_stream` (built from 0001–0048) on every kernel.

What 0048 changed against the 0047 variant: it adds two per-stream configfs budgets, `rx_repost_batch` and
`tx_submit_batch` (0..64 frames, default 0), that publish RX reposts and TX submissions in batches with one
doorbell. With the default 0 every frame is submitted individually as before; batching was measured with no
bandwidth change and stays off ([host-descriptor-batching.md](host-descriptor-batching.md)). 0048 changes the
stream module only: the `thunderbolt` core module is the same, so a 0048 stream module can be built for a kernel
whose core came from 0001–0047.

Which module each kernel loads:

| Kernel | Stream module at boot | Extra step |
|---|---|---|
| PerfOpt kernel | 0048 variant, shipped in the kernel package's own module tree | none |
| Plain kernel built from 0001–0048 | 0048 variant, built in-tree | none |
| Plain kernel built from 0001–0047 (our plain kernel) | 0047 variant unless the 0048 module is installed | install the 0048 module once into that kernel's module tree (below) |

Permanent install of the 0048 module for a 0001–0047 kernel (once per kernel, as root):

1. Build `drivers/thunderbolt` with 0048 applied from the same tree, configuration and toolchain as that kernel.
2. Check the built module before using it: its hash, its srcversion (`modinfo -F srcversion thunderbolt_stream.ko`)
   and its vermagic, which must equal that kernel's release.
3. The kernel's initramfs must not carry `thunderbolt_stream` (check with `lsinitramfs`); if it does, do not
   continue, since the boot would load the old copy from the initramfs.
4. Replace that kernel's own `thunderbolt_stream.ko` in `/lib/modules/<release>/` with the new build, keep the
   old file next to it as the rollback copy (record its hash), and run `depmod -a <release>`.
5. The change takes effect at the next boot of that kernel; the running module is never reloaded. After that
   boot, `/sys/module/thunderbolt_stream/srcversion` must equal the new build's srcversion.

Rollback: put the kept original back (its hash must match the recorded one), run `depmod -a <release>`, reboot.
No manual module reload is part of normal operation: the lab service loads whatever module the running kernel's
tree provides.

**Verify after boot:** `cat /sys/module/thunderbolt_stream/srcversion` must equal the srcversion recorded for
that build. Record it when you build: `modinfo -F srcversion <build>/drivers/thunderbolt/thunderbolt_stream.ko`.
A srcversion depends on the sources and configuration, so record your own; do not reuse ours. Example, *our
build*: 0048 variant `3A05C2B1A13ABAEF9B767EF`, 0047 variant `36F020414F7D1FCB014729D`, core `thunderbolt`
`C6FAC8417FE09BD10F37285` (the same in our plain and PerfOpt kernels).

## 4. Boot options and system settings

| Setting | Where | Why |
|---|---|---|
| `kho=off` | a drop-in under `/etc/default/grub.d/` that appends it to the kernel command line, then `update-grub` | disables Kexec HandOver, whose scratch area is otherwise reserved at boot; we keep that memory available to the GPU |
| `options ttm pages_limit=30720000` | `/etc/modprobe.d/ttm.conf`, then rebuild the initramfs if TTM is loaded there | the GTT limit we use: 30720000 pages x 4 KiB = 117.19 GiB of system memory. If TTM is built in, use `ttm.pages_limit=` on the command line instead |
| `thunderbolt_net` install rule | [`tools/lab/usb4stream-dedicated.conf`](../tools/lab/usb4stream-dedicated.conf) in `/etc/modprobe.d/` | keeps both data ring pairs of each NHI for USB4STREAM on lab kernels; stock kernels keep USB4 networking |
| `usb4stream-lab.service` | [`tools/lab/`](../tools/lab/), enabled | creates the streams and the layout file at boot. Its `ConditionKernelVersion` matches the lab `LOCALVERSION` prefix, so a stock kernel skips it; the optional reviewed-kernel drop-in (`tools/lab/reviewed-kernel-dropin <release>`) limits it to one exact release |
| `usb4stream-describe-refresh.service` + `90-usb4stream-describe.rules` | `tools/lab/`, optional | rewrites the layout file after a cable or peer change |
| Default boot entry | `GRUB_DEFAULT` in a `/etc/default/grub.d/` drop-in | set to a kernel only after it passed a one-shot test boot |

Keep every node's settings identical. Kernel kind specific: the PerfOpt setting (`amdgpu.iommu_perfopt`) and
the kdump/kexec rule are in [PERFOPT.md](PERFOPT.md). Install commands for the lab files:
[INSTALL-AND-TEST.md](INSTALL-AND-TEST.md) section 5.

### Testing a new kernel: one-shot boot

1. Install the new kernel next to the current one; leave `GRUB_DEFAULT` unchanged.
2. `sudo grub-reboot '<menu entry of the new kernel>'` and reboot. GRUB boots it once and consumes the entry.
3. Run `tools/lab/node_check.sh` (section 6). If it passes on both nodes, make the kernel the default (for the
   PerfOpt kernel, read the lifecycle caveat in [PERFOPT.md](PERFOPT.md) first): set `GRUB_DEFAULT` in a
   `/etc/default/grub.d/` drop-in, run `update-grub`, and only do it from a boot of that kernel that passed the
   check. To undo, remove the drop-in and run `update-grub` again.
4. To roll back, reboot: the default entry is still the previous kernel. If the test boot does not come up,
   power-cycle; the next boot is the default kernel.

Once a kernel is the default, a power cycle boots it again. With a hidden GRUB menu and a zero timeout there is no
chance to pick another entry, so recovering from a default kernel that hangs at boot needs a console (keyboard
and display) to reach the GRUB menu.

This needs a GRUB environment block that GRUB can write at boot (plain ext4 `/boot`, no LVM, RAID, btrfs or
ZFS). After every test boot `sudo grub-editenv list` must show no `next_entry`, or an empty `next_entry=` (GRUB's
in-place save leaves that line); a non-empty value means a one-shot boot is still pending.

## 5. PerfOpt

AMD's IOMMU PerfOpt lets the integrated GPU skip IOMMU translation while the IOMMU stays on and both NHIs stay
`DMA-FQ`; it needs the AMD IOMMU series above and trades the GPU's own DMA containment for speed. What it is, the
security trade-off, how to enable, verify and roll back: [PERFOPT.md](PERFOPT.md).

## 6. After every boot / reboot: verify

On each node:

```sh
EXPECT_STREAM_SRCVERSION=<srcversion recorded for this build> EXPECT_PERFOPT=<on|off|absent> \
    tools/lab/node_check.sh
```

`EXPECT_LINKS` (default 2, one per cable) sets the number of links and USB4 domains expected; `ALLOW_GEN2=1`
accepts links below Gen3 with a `WARN`. Without `EXPECT_STREAM_SRCVERSION` the srcversion is only printed, with a
`WARN`.

Read-only, no root needed, but the kernel log must be readable (root, or group `adm`/`systemd-journal`):
without it the log-based checks FAIL instead of passing. It prints one `PASS`/`FAIL`/`INFO`/`WARN` line per
check and exits non-zero on any `FAIL`:

| Check | PASS when |
|---|---|
| Kernel release and kind | reported; *PerfOpt kernel* when `amdgpu.iommu_perfopt` exists (or a "PerfOpt armed" line is logged), *plain kernel* otherwise |
| Command line | `kho=off` present; no `iommu_perfopt` (it belongs in a modprobe file) |
| PerfOpt state | parameter not `0` and "PerfOpt armed" logged (`on`); or `0` and not logged (`off`); no parameter and not logged (`absent`); equal to `EXPECT_PERFOPT` if set; no PerfOpt error lines |
| Stream module | `thunderbolt_stream` loaded; the configfs streams have the 0048 attributes `rx_repost_batch` and `tx_submit_batch`; srcversion equal to `EXPECT_STREAM_SRCVERSION` |
| Links | exactly `EXPECT_LINKS` links; every link (each `rx_speed`/`tx_speed` with its lanes) at 20 Gb/s per lane x 2 lanes (Gen3, 40 Gb/s). `ALLOW_GEN2=1` turns a slower link into `WARN` |
| IOMMU | GPU group type reported (must be `identity` when PerfOpt is armed); exactly `EXPECT_LINKS` USB4 domains; every NHI group `DMA-FQ`; `iommu_dma_protection` = 1 for every domain |
| AMD-Vi | no IOMMU fault or error lines in this boot's kernel log |
| Lab service and layout | service active; `/run/usb4stream-layout.json` exists and its stream mapping (stream, device, cable) equals a fresh read-only `usb4stream-describe` pass that reads configfs (never the file itself) |

Run it again after the nodes have exchanged traffic over both cables: AMD-Vi faults can appear only under
traffic.

### A link below Gen3

Link speed is negotiated when the cable is attached. Reloading the modules does not renegotiate it (in our tests
the negotiated speed was kept across module reloads), so it is not a fix.
If a link reports 10 Gb/s per lane (Gen2) or one lane:

1. Unplug the USB-C plug at one end of that cable and plug it back in (optionally rotated), with no stream open
   on that cable.
2. Re-run `tools/lab/node_check.sh` on both nodes. The layout file follows the change only with the optional
   refresh rule installed; otherwise the layout check reports the difference until the next boot.
3. If it stays below Gen3, see the link training note in the README [Limitations](../README.md#limitations): a
   port whose router does not advertise Gen3 cannot be retrained, and `thunderbolt.gen3_retrain` (0035, opt-in)
   only acts when every side advertises Gen3.

## 7. Optional: NPU runtime (XRT)

The Ryzen AI Max+ 395 also has an XDNA NPU. Its kernel driver, `amdxdna`, is part of the kernel (no DKMS
module); with it loaded the device node `/dev/accel/accel0` is present. To run NPU programs, a node
also needs the XRT userspace. It is optional: the USB4 driver, the stream module and the inference engine do
not use the NPU, and a node without XRT passes every check in section 6. Install it only on nodes that will run
NPU work (for example NPU throughput or concurrency measurements next to the GPU).

Install from the Ubuntu 26.04 `universe` packages (XRT 2.21.75, about 10 MB), as root, on each node:

    apt-get install -y libxrt2 libxrt-npu2 libxrt-utils libxrt-utils-npu libxrt-dev
    printf '* soft memlock unlimited\n* hard memlock unlimited\n' > /etc/security/limits.d/99-npu-memlock.conf

NPU buffers are pinned memory, hence the unlimited `memlock` limit. It applies to new login sessions: log out
and back in (or open a new SSH session) before the check.

Check, as an ordinary user:

    ulimit -l            # unlimited
    xrt-smi examine      # lists the NPU (NPU Strix Halo), its firmware version and the XRT version

Remove: `apt-get remove libxrt2 libxrt-npu2 libxrt-utils libxrt-utils-npu libxrt-dev` and delete
`/etc/security/limits.d/99-npu-memlock.conf`.
