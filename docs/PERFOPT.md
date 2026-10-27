# GPU IOMMU PerfOpt on Strix Halo (optional, measured)

**Status:** optional. Not the default kernel recommendation. Measured exact and faster on one of our two nodes
(the two-node run is pending); the default stays the kernel without PerfOpt until the upstream lifecycle rework
below is in the kernel we ship.

## What it is

AMD's IOMMU "Performance Optimization" (PerfOpt) lets the **integrated GPU** reach system memory without IOMMU
address translation. It is only enabled when the GPU is already in an *identity* IOMMU domain. The IOMMU itself
stays **on**: every other device behind it keeps its own translated domain, including both USB4 host
controllers (NHIs) used by this driver, which stay `DMA-FQ`.

It comes from the `amd/amd-vi` series queued in the IOMMU tree for Linux 7.4. The two PerfOpt patches are:

- `iommu/amd: Add PerfOpt IOMMU performance optimization support`
- `drm/amdgpu: Enable PerfOpt ... when GPU in identity domain`

The amdgpu module parameter `amdgpu.iommu_perfopt` controls it (enabled by default on supported devices,
`0` disables it). A later upstream revision moves the policy into the IOMMU attach path and drops the
parameter; check your kernel version before relying on the parameter name.

## Why

It removes the GPU's DMA translation cost. Measured on one node (Ryzen AI Max+ 395, same kernel build,
PerfOpt on vs `amdgpu.iommu_perfopt=0`, interleaved boots, tokens identical in every arm):

| Workload | PerfOpt on vs off (same kernel) | Notes |
|---|---|---|
| Qwen3.8-Flash-Next prefill, 32K prompt | **+2.9 % / +3.1 %** (two repeats) | same busy power, equal or lower GPU clock; +3.6..3.8 % vs our previous kernel under matched thermal state |
| DeepSeek V4.1 two-node decode | within ±1 % with only one node on PerfOpt | the two-node number needs both nodes on PerfOpt and is pending |

## Security trade-off, in plain words

With PerfOpt armed, **the GPU's own memory accesses are no longer checked by the IOMMU.** The GPU's own page
tables (GPUVM/GART) still confine normal workloads, but the IOMMU is no longer a second line of defence: a GPU
driver or firmware bug, or a workload that escapes the GPU's own page tables, is no longer stopped by the IOMMU
and could reach any system memory. A blocked IOMMU domain no longer stops the GPU either.

The other devices stay behind the IOMMU: the USB4 controllers and the rest keep their translated domains, and
`iommu_dma_protection` stays `1`. That the USB4 controllers stay translated is what the kernel reports and what
our tests showed (no IOMMU faults, exact traffic over both cables). AMD has not published the exact hardware
rule for which requester skips translation, so this is tested, not proven.

This is a deliberate trade of the GPU's DMA containment for lower GPU DMA latency. It goes one step beyond the
identity domain the GPU already uses: identity skips address remapping, but the IOMMU still handles each access
and can block the GPU; PerfOpt removes that too. **Do not use `amd_iommu=off` or `iommu=pt`** to get the same
gain: those also remove the protection of the USB4 controllers, which the quarantine and reclamation logic of
this driver depends on.

The upstream version we measured has known lifecycle gaps: a failed clear before re-attaching the GPU to a
translated or blocked domain is logged and ignored (GPU unbind, VFIO passthrough), and kexec/crash kernels can
inherit the setting. Suspend and amdgpu reload are handled but untested by us. Use PerfOpt only on a dedicated
inference host that does none of the risky operations (disable kdump/kexec tools there), and prefer a kernel that
carries the reworked upstream version once it is released.

## How to enable

1. Use a kernel with the queued `amd/amd-vi` series (13 commits, including the two PerfOpt patches and the one
   that forces the identity domain only for the APU GPU; we applied them onto our 7.3 build, with this
   repository's USB4 series unchanged). Give the build its own `LOCALVERSION` and keep your current kernel as the
   default boot entry.
2. The GPU must be in an identity domain and the NHIs must stay `DMA-FQ`. With this series only the GPU should be
   in an identity group; list them with `grep -l identity /sys/kernel/iommu_groups/*/type`.
3. Boot the new kernel **once** first (for example `sudo grub-reboot <entry>` followed by a reboot), verify, and
   only then consider making it the default.
4. PerfOpt is on by default in that kernel. To keep the kernel but turn PerfOpt off, set
   `options amdgpu iommu_perfopt=0` in a modprobe file and rebuild that kernel's initramfs. Do not put
   `iommu_perfopt` on the kernel command line.

Changing the kernel, GRUB and modprobe files needs root; do it from a console or SSH session you can recover.

## How to verify

These read-only checks need read access to the kernel log (root, or membership in `adm`/`systemd-journal`).
First confirm the log is readable: `journalctl -k -b | grep -m1 'Linux version'` must print a line; if it does
not, the checks below prove nothing. After boot, all of these must hold:

```sh
journalctl -k -b | grep -i perfopt                     # expect "... AMD-Vi: PerfOpt armed on IOMMU0"
journalctl -k -b | grep -iE 'PerfOpt.*(fail|error)'    # empty
cat /sys/module/amdgpu/parameters/iommu_perfopt        # not 0
grep -c iommu_perfopt /proc/cmdline                    # 0 (the setting comes from modprobe, not the cmdline)
readlink /sys/bus/pci/devices/<GPU>/driver             # ends in amdgpu
cat /sys/bus/pci/devices/<GPU>/iommu_group/type        # identity
cat /sys/bus/pci/devices/<NHI>/iommu_group/type        # DMA-FQ, for every USB4 controller
cat /sys/bus/thunderbolt/devices/domain*/iommu_dma_protection   # 1 for every domain
journalctl -k -b | grep -iE 'IO_PAGE_FAULT|INVALID_DEVICE_REQUEST|ILLEGAL_(DEV_TABLE|COMMAND)|HARDWARE_ERROR|event logged|Completion-Wait loop timed out|AMD-Vi.*(fail|error|timeout|disabled)'   # empty, also after traffic
```

`<NHI>` is the PCI parent of each `readlink -f /sys/bus/thunderbolt/devices/domain*`. Run the last check again
after the two nodes have exchanged traffic over both cables.

## How to disable or roll back

- **Keep the kernel, turn PerfOpt off:** `options amdgpu iommu_perfopt=0` in a modprobe file, rebuild that
  kernel's initramfs, reboot; verify that the parameter reads `0` and no "PerfOpt armed" line appears.
- **Go back to the previous kernel:** if you used a one-shot boot, simply reboot (the default entry is
  unchanged). Otherwise select the previous entry in GRUB, reboot, and remove the PerfOpt kernel package when no
  longer needed.
- If a test boot does not come up, power-cycle: a one-shot entry is consumed, so the next boot is the default
  kernel. This needs a GRUB environment block that GRUB can write at boot (plain ext4 `/boot`, no LVM, RAID,
  btrfs or ZFS). After every test boot, `sudo grub-editenv list` must show no `next_entry`, or an empty
  `next_entry=` (GRUB's in-place save leaves that line); a non-empty value means a one-shot boot is pending.

Do not rebuild other kernels' initramfs images (for example `update-initramfs -u -k all`) while an
`iommu_perfopt=0` modprobe file exists only for the test kernel: it would be copied into them too. It is harmless
there (unknown module options are ignored) but the images would no longer match their build.
