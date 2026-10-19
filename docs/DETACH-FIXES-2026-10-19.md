# 0017: streams and services that outlive their connection (2026-10-19)

State: implemented, offline-tested (unit tests under ASan/UBSan) and
compile-tested against the pinned 7.3-rc5 with the nodes' running
configuration. **Not run on hardware yet**; see the checks at the end.

Sources: the review of the 0012–0016 branch; the cluster1 vmcore of
2026-10-19 13:50; the live cable pull at 14:24:24 (cluster2 warnings,
cluster1 NHI1 wedge).

## 1. The RAM-ring stream bug

**Bug.** A `/dev/tbstreamN` file using RAM rings keeps its rings when its
stream goes away:

- `tbstream_remove()` → `tbstream_dev_detach_stream()` only disconnected a
  GPU context. It then set `sdev->stream = NULL` and left the RAM rings
  running.
- configfs `rmdir` took the item off the group list, so a later service
  removal could no longer find it. It did not stop the rings either.
- The stream service driver had no `.shutdown`.

The rings then outlived their paths, their service and possibly the NHI and
its MMIO. The file's last close stopped them much later, through a ring the
hardware had already torn down. cluster2 showed exactly this at 14:25:37, a
minute after the cable pull: `WARNING nhi.c:157 ring_interrupt_active:
interrupt for TX ring 2 is already disabled`, then the same for TX ring 1,
in `tb_ring_stop()` ← `tbstream_dev_stop()` ← `tbstream_dev_fops_release()`.
A reconnect also attached the new stream to the endpoint, so the stale file
became "valid" again with no rings behind it.

**Fix.**

- *Detach stops RAM rings.* With files open and no GPU context,
  `tbstream_dev_detach_stream()` sends CLOSE if the peer is still there
  (rmdir, module unload, shutdown) and then stops the rings. While the
  stream is still known, `tbstream_dev_stop()` disables the paths; for an
  unplugged XDomain the CM part is skipped, as before. Finally it marks the
  device `detached`. GPU contexts keep their existing quarantine path.
- *Stale files fail cleanly.* `read`/`write` return `-ENODEV` and `poll`
  returns `EPOLLHUP | EPOLLERR`. None of them touches a ring, including
  after a reconnect and including a reader that slept or spun in `read()`
  across the detach. The detach takes `rx_lock`, so it waits for a
  spinner. Close of a stale file neither stops anything nor sends CLOSE; it
  only clears `detached` when the last stale file goes. `tbstream_dev_stop()`
  is a no-op without rings, so suspend followed by a failed resume no longer
  stops twice.
- *Reconnect does not reuse a stale endpoint.* The endpoint attaches to the
  new stream, but `open()` returns `-EBUSY` while stale files remain. A new
  open would otherwise share their count and skip starting rings. Resume does
  not restart a detached endpoint.
- *rmdir* calls the same detach after taking the item off the list. The
  live peer gets CLOSE, the paths are disabled, the HopIDs are released and
  the published property directory is withdrawn.
- *`.shutdown`* (`tbstream_shutdown`) detaches like removal does. Reboot and
  power-off do not remove services, and the NHI stops its DMA next.

**Kept on purpose.** A reader that wakes after the CM marked the XDomain
unplugged, but before the service is removed, still gets `-ENXIO` as before.
Every later call gets `-ENODEV`.

## 2. XDomains and services outliving their domain (the vmcore)

**Finding.** The vmcore of cluster1 (13:50, kernel
`7.3.0-rc5-strix-gpu-bosgame-20261014`, build ID
`c4928f9cf0e4b28e2a657cdf9282e6cc874c1ac7`) faults at
`device_shutdown+0x148`. That instruction follows the call
`dev->bus->shutdown(dev)` (`core.c:4925`, `bus_type+0x50`). The device is
`0-2.0`, a `tb_service` under XDomain `0-2`:

- it is still registered (in sysfs, on the bus klist) with no driver;
- its `bus` and `type` point into a thunderbolt module that had been unloaded;
- the reloaded module's `tb_quirks[]` now occupies that memory, so `bus->shutdown`
  read `0x57818087` (the quirk IDs `0x8087:0x5781`) and the CPU jumped there.

The domain object itself is gone from the dump.

**Cause.** Since 7.3, XDomains are unregistered outside `tb->lock` by
`tb_domain_unregister_unplugged_xdomains()`. The hotplug handler calls it,
but domain removal (NHI unbind, failed reprobe, module unload, reboot) does
not. `tb_stop()` → `tb_switch_remove()` only marks each XDomain unplugged and
calls `tb_xdomain_remove()`. The XDomain and its services therefore stay
registered after the domain, and after the module unload, are gone. Their
service drivers never run either, so in this tree the RAM rings (section 1)
also stayed up across `nhi_shutdown()`.

**Fix (core).** A new CM hook, `remove_xdomains`, is implemented by the
software CM as `tb_remove_xdomains()`. Under `tb->lock`, it stops hotplug
processing and removes every XDomain exactly as `tb_switch_remove()` would,
but while the routers still exist. `tb_domain_remove()` then drops the lock
and unregisters the XDomains on the domain's ordered workqueue. The hotplug
handler unregisters from there too, so nothing is unregistered twice.
`tb_stop()` runs only after that. As a result:

- no service, XDomain or bus pointer outlives the domain or the module;
- the unregistration happens while the parent routers are registered, so no
  child is removed after its sysfs parent;
- service drivers stop their rings before `tb_stop()` deactivates the DMA
  paths. This is the right order for teardown on unbind and reboot. A
  physical unplug cannot be ordered this way: the link is already gone.

A CM without the hook (the firmware CM) keeps the previous order.

`nhi_probe()` also no longer waits forever for the domain after a failed
`tb_domain_add()`. It uses the same bounded `tb_domain_wait_released()` as
remove and shutdown since 0013. The review had found this wait still
unbounded.

## 3. The cluster1 NHI1 wedge at 14:24:24

**Observed.** NHI1 (`c7:00.6`) stopped answering control-channel reads at
the moment the cable was pulled. Even route 0, the host router itself, did
not answer, and the CM was still tearing down the DMA tunnels (`hop
deactivation failed` for hop 0 of `0:1` index 1 and 2, `0:2` index 9). The
same NHI later failed every reprobe (`failed to allocate host router`,
`-ETIMEDOUT`). A PCI ACPI reset did not help either. cluster2's NHI on the
same cable survived, with the warnings of section 1.

**Assessment.**

- **Most likely: a hardware TX hang on teardown under load.** Upstream marks
  every Strix Halo NHI (`1022:158d/158e`, `M70H`) with
  `QUIRK_RESET_DMA_ON_TEARDOWN`: "These AMD hosts may hang the Tx ring when
  the DMA paths are torn down". The control channel is ring 0 of the same
  DMA engine. A hung TX side therefore also explains why every config read,
  even to route 0, timed out. The timeouts start with the first hop read,
  before any stream code ran, so the trigger was the unplug under traffic
  plus the CM teardown. It was not our close path.
- **0001 removed the only automatic recovery.** Upstream resets the host
  interface after each `disconnect_xdomain_paths()` on these NHIs. That
  reset also wipes the rings of every other user of the NHI (other streams,
  USB4NET, GPU sessions), which is why 0001 removed it (review H-0b). Without
  it nothing ever reset the hung interface. Nor did the probe: `nhi_reset()`
  only writes HRR on v2 routers, and these report `REG_CAPS = 3`.
- **Our RAM-ring bug did not start it, but it made it worse.** The stale
  rings kept the TX DMA of a removed path armed for over a minute. They also
  would have made an interface reset unsafe. 0017 stops them at service
  removal.
- **Not supported by the evidence:** the hotplug handler racing open
  streams. Service removal is serialized after the CM teardown by the
  deferred unregistration. Nor does a DMA path get torn down by our code
  while its rings run, except at the unplug itself, which no driver can
  order.

**Prevention and recovery in 0017.**

- Rings stop at service removal (section 1) and before path teardown on
  unbind and reboot (section 2).
- **Host interface reset at probe** on the quirked AMD v1 NHIs, under
  `host_reset=Y` (the default). Nothing runs at probe, so no other user's
  rings are lost. If the wedge is the interface TX hang the quirk describes,
  an unbind and bind of that NHI can now recover it without a reboot.
  **Unverified on hardware.** If the router itself, not the interface, is
  wedged, only a reboot helps.

**Not done, proposed.** A teardown reset that is safe for other users:
restore upstream's post-teardown interface reset only when the NHI has no
other non-control ring (the same admission test as the explicit
`tb_domain_reset_dma()`), after an unplugged XDomain's paths are gone. This
reintroduces an automatic reset, which the series' design has rejected so
far. It needs a decision, and a hardware reproduction first.

**Remaining warning.** After a physical unplug, 0017 stops the rings at
service removal, within about a second rather than at close. If the AMD
interface itself disables the TX ring interrupt on teardown (one reading of
cluster2's warnings), `tb_ring_stop()` still logs `interrupt for TX ring N is
already disabled` once per ring at that point. The warning is left in place
because it is a useful hardware signal.

## 4. `host_reset` documentation fix

`docs/REVIEW-FIXES-2026-10-19.md` used to recommend reloading with
`host_reset=Y` to get HRR. On these NHIs `REG_CAPS = 3`, so the version is
0 and `nhi_reset()` returned before writing HRR. The section is corrected
there.

## 5. Tests

`tests/test_stream_host_path.py` runs the actual stream functions against
ring, CM and file models (new cases in `tests/stream_detach_cases.h`):

| Case | Checks |
|---|---|
| `detach_stops_open_ram_rings` | interrupts, `rx_napi` with spin, `busy_poll`: rings, pages, mappings and tunnels gone at removal; no CLOSE to a gone peer; read/write `-ENODEV`, poll HUP/ERR; close without a second stop |
| `reconnect_refuses_stale_endpoint` | new stream attached, the stale file never reaches it; new open `-EBUSY` until the stale file closes, then opens on the new peer |
| `blocked_reader_fails_cleanly` | reader asleep or spinning in `read()` at unplug (`-ENXIO` or `-ENODEV`) or shutdown (`-ENODEV`) |
| `rmdir_with_open_file` | CLOSE to the live peer, paths disabled, HopIDs released, properties withdrawn; later removal does not touch it |
| `detach_without_open_files` | unchanged: only HopIDs released |
| `stop_is_idempotent` | suspend + failed resume, then close (with or without detach) |
| `shutdown_detaches`, `shutdown_is_registered` | `.shutdown` stops the rings and is wired in the driver |

`tests/test_domain_teardown.py` runs the actual `tb_domain_remove()`,
`tb_remove_xdomains()` and `nhi_reset()`:

- the XDomains are unregistered once, without `tb->lock`, on the domain
  workqueue, with parents and tunnels still present, and before the paths
  are deactivated (rings already stopped);
- a hotplug work queued ahead does not cause a double unregistration;
- a CM without the hook keeps the old order;
- the software CM registers the hook;
- v1 quirked: interface reset written, flushed, followed by tHIReset;
- v1 unquirked or `host_reset=N`: nothing written;
- v2: HRR only;
- the failed probe uses the bounded wait;
- the Strix Halo IDs are quirked.

Each fix was also removed one at a time (mutation check): every removal makes
at least one of these tests fail.

## 6. Hardware checks before installing

One node at a time, current kernel as default, peer stable, management over a
separate network:

1. **Probe.** `dmesg` shows `host interface reset` once per NHI, then Gen3 × 2
   on both links. Then run the README regression campaigns (bandwidth,
   `pingpong`, 80 open/close cycles, GPU one-way and duplex).
2. **Unplug with open RAM streams.** Run traffic on all streams of one
   cable and pull it. The stream processes get `-ENXIO`/`-ENODEV`, `poll`
   reports HUP. `dmesg` shows no oops and at most one "already disabled" line
   per ring, at service removal rather than at process exit. After replug,
   reopening a stream returns `-EBUSY` until the old process exits, then
   works. Record whether the control channel survives (`-ETIMEDOUT` lines).
3. **rmdir with an open stream.** The peer's reader gets EOF, and
   `/proc/interrupts` shows the ring vectors gone right away.
4. **Unbind with XDomains connected.** `echo <bdf> > .../unbind`: `host
   disconnected` is logged for each XDomain, and `/sys/bus/thunderbolt/devices`
   has no entry left for that domain. Then `bind`. Repeat with
   `modprobe -r thunderbolt_stream thunderbolt_net thunderbolt`. Finally
   reboot with kdump armed: no oops in `device_shutdown`.
5. **Wedge recovery (opportunistic).** If an NHI ever wedges like NHI1 did,
   try unbind and bind before rebooting, and record whether the interface
   reset brings back the host router.
