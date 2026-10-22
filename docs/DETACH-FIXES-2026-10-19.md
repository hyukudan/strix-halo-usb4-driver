# 0017–0019: streams and services that outlive their connection, AMD teardown recovery, bounded open (2026-10-19)

State: implemented, offline-tested (unit tests under ASan/UBSan) and
compile-tested against the pinned 7.3-rc5 with the nodes' running
configuration. **Not run on hardware yet**; see the checks at the end.

Sources: the review of the 0012–0016 branch; the node A vmcore of
2026-10-19 13:50; the live cable pull at 14:24:24 (node B warnings,
node A NHI1 wedge).

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
hardware had already torn down. Node B showed exactly this at 14:25:37, a
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

**Finding.** The vmcore of node A (13:50, kernel
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

## 3. The node A NHI1 wedge at 14:24:24

**Observed.** NHI1 (`c7:00.6`) stopped answering control-channel reads at
the moment the cable was pulled. Even route 0, the host router itself, did
not answer, and the CM was still tearing down the DMA tunnels (`hop
deactivation failed` for hop 0 of `0:1` index 1 and 2, `0:2` index 9). The
same NHI later failed every reprobe (`failed to allocate host router`,
`-ETIMEDOUT`). A PCI ACPI reset did not help either. Node B's NHI on the
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

**Done in 0018 (decided 2026-10-19).** A teardown reset that is safe for
other users: upstream's post-teardown interface reset is restored, but only
when the NHI is idle. See section 7.

**Remaining warning.** After a physical unplug, 0017 stops the rings at
service removal, within about a second rather than at close. If the AMD
interface itself disables the TX ring interrupt on teardown (one reading of
node B's warnings), `tb_ring_stop()` still logs `interrupt for TX ring N is
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
separate network. Items 6–9 also cover 0018. Record the full `dmesg` of both
nodes for each item.

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
6. **Unplug under traffic on one cable while the other cable keeps traffic
   (0017 + 0018).** Run RAM traffic on the streams of both cables (each cable
   on its own NHI) and pull one cable. On the pulled side:
   - the stream processes fail as in item 2;
   - within about 1 s, `host interface reset after DMA teardown (attempt N)`
     appears for that NHI's domain;
   - the control channel keeps answering (no endless `timeout reading config
     space`);
   - the cable can be replugged and the streams reopened without a reboot.

   On the other cable, traffic continues without a single error or stall
   (its NHI is not reset). Repeat with a second stream, or a GPU session,
   still open on the pulled NHI. Expect no reset, then after about 5 s one
   `no host interface reset after DMA teardown: NHI in use after 10
   attempts (...)` line with the ring counts.
7. **Reboot with streams open, modules loaded.** Run RAM traffic (and a
   `gpu_direct` session) on both cables and run `systemctl reboot` without
   stopping the processes or unloading any module, with kdump armed. The
   reboot completes, with no oops in `device_shutdown` or in the stream
   release path. Each stream is stopped by `.shutdown` or by service
   removal, before the NHI.
8. **rmmod with live XDomains.** With both peers connected and no stream
   open, run `modprobe -r thunderbolt_stream thunderbolt_net thunderbolt`.
   Check:
   - `host disconnected` for each XDomain;
   - nothing left in `/sys/bus/thunderbolt/devices`;
   - no "TX/RX ring N is still active" warning from `nhi_shutdown()`;
   - no teardown reset logged after the domain removal started.

   Then `modprobe thunderbolt` (probe: `host interface reset`, Gen3 × 2).
   Repeat once with a stream open: the module must refuse to unload while
   the file holds it.
9. **unbind/bind of one NHI.** With XDomains connected and streams open on
   that NHI, unbind it:
   - it returns within 30 s;
   - the streams fail with `-ENODEV`;
   - the XDomains and services are unregistered (sysfs empty for that domain).

   Then bind: `host interface reset`, Gen3 × 2, and the streams reopen
   (after their old files are closed). The other NHI keeps its traffic
   throughout.
10. **open() without a connection (0019).**
    - With one NHI unbound, or its domain gone, `open()` of its endpoints
      returns `ENODEV` immediately.
    - With the cable unplugged, it returns `ENXIO` immediately.
    - With the peer connected but its `thunderbolt_stream` not loaded: a
      blocking open returns `ETIMEDOUT` after 30 s (or `open_timeout_ms`),
      `O_NONBLOCK` returns `EAGAIN`, and Ctrl-C ends the wait at once. Loading
      the peer's module during the wait makes the open succeed.
    - Nothing stays in `tbstream_dev_fops_open` (check `wchan`).

## 7. 0018: automatic recovery after a DMA teardown, only on an idle NHI

**Decision (2026-10-19).** Restore upstream's recovery of the AMD TX hang
after a DMA path teardown, but never at the cost of another user of the NHI.

**Implementation** (`dma_recovery.c`, called from `domain.c` and `tb.c`):

- **Trigger.** Every successful `tb_domain_disconnect_xdomain_paths()`
  schedules it: a stream close, a failed open, a service removal or
  `gpu_direct` teardown. So does the CM's own teardown of an unplugged
  XDomain's paths in the hotplug handler, for when no service driver tears
  them down. Only on the AMD NHIs of `nhi_dma_reset_supported()` (Strix
  Halo included), only while the domain is not going away, and only with the
  new `thunderbolt.teardown_reset` parameter on (`Y` by default, writable at
  runtime).
- **Admission.** The delayed work (100 ms) calls the existing reset with an
  empty owner set. It is the same admission as the explicit
  `tb_domain_reset_dma()`, and the reset happens only if:
  - no ring is allocated on the NHI besides the control ring, which the
    reset itself stops and restarts. Rings of another stream, of USB4NET or
    of a GPU context (open or quarantined) all count, idle or not;
  - the CM has no DMA tunnel left;
  - no other reset is in progress;
  - the interface is v1 (`REG_CAPS` version < 2);
  - the device is present and registered.
- **Never waits for the domain lock.** It uses `mutex_trylock()`, so a CM
  stuck in control-channel timeouts cannot block it, and it never blocks
  the CM.
- **Bounded.** Up to 10 attempts, 500 ms apart: about 5 s, enough for the
  tearing-down owner to free its rings. A new teardown restarts the budget.
  The work never runs during or after domain removal (`dma_shutdown`
  refuses scheduling, removal and release cancel it), or across system
  suspend or hibernation (cancelled in `tb_domain_suspend()` and
  `tb_domain_freeze_noirq()`).
- **dmesg** (domain device, e.g. `thunderbolt domain1:`):
  - success: `host interface reset after DMA teardown (attempt N)`;
  - other users: `no host interface reset after DMA teardown: NHI in use
    after 10 attempts (T TX and R RX rings allocated besides control; a DMA
    tunnel or a reset in progress also prevents it)`;
  - lock never free: the same line with `domain lock busy`;
  - reset failed (`REG_CAPS` read-back mismatch or the control channel did
    not restart): `host interface reset after DMA teardown failed: <errno>,
    new rings are refused until the NHI is probed again`. The series' rule
    applies: the hardware state is no longer trusted, and recovery needs an
    unbind and bind.

**Limits.**

- It recovers a hang of the host interface (DMA rings, E2E, control
  ring). A wedged router or hop table is not reset; that still needs a
  reboot.
- It is skipped whenever anything else still uses the NHI, including a
  GPU context that stays quarantined after an unplug. In that case the
  warning says so, and recovery needs closing or reaping it, then an
  unbind and bind, which runs the probe-time reset of 0017.
- Both are unverified on hardware: section 6, items 6–9.

**Tests** (`tests/dma_teardown_cases.h` in `test_dma_recovery`, run against
the actual `dma_recovery.c`):

| Case | Checks |
|---|---|
| `teardown_reset_idle` | only user torn down: one reset, control channel restarted, info line |
| `teardown_reset_other_stream` | **another stream active on the same NHI: no reset**, 10 attempts, one warning with the ring counts; same for a lone idle ring |
| `teardown_reset_waits_for_owner` | the owner's own rings freed after the first attempt: reset at attempt 2 |
| `teardown_reset_lock_busy` | domain lock held: no wait, reset once free; held throughout: bounded, `domain lock busy` |
| `teardown_reset_tunnel_veto` | a DMA tunnel still in the CM: no reset |
| `teardown_reset_gated` | other vendor, parameter off, `dma_shutdown`, `going_away`: not scheduled; v2: refused, no retry; removal after queuing: one `-ENODEV`, no retry |
| `teardown_reset_failure` | failed reset: error line, admission stays closed, no retry |
| `teardown_reset_budget_restarts` | a new teardown restarts the 10 attempts |

`test_domain_teardown` adds `teardown_reset_wiring`, which checks where it is
scheduled and cancelled. Its removal-order case now also checks that the
work is cancelled before the DMA owners are retired. Each part was removed
one at a time (mutation check); every removal makes at least one test fail.

## 8. 0019: open() never waits for a connection that cannot appear

**Incident (2026-10-19 15:21).** A two-node run sat 27 minutes in
`tbstream_dev_fops_open` (`wchan`) on `/dev/tbstream1` and `/dev/tbstream3`.
Their configfs endpoints belonged to domain 1, which no longer existed (NHI1
of node A wedged, and the second cable of node B had no peer). An open
with no stream attached slept until a stream appeared or a signal arrived.

**Fix.**

- **Core: `tb_xdomain_presence(domain, route)`** (exported). It looks up
  `domainN` and the XDomain `N-route` on the bus by name, so it needs no
  domain reference. It returns:
  - `-ENODEV` without the domain;
  - `-ENXIO` without the XDomain, when the XDomain is unplugged, or when the
    name belongs to a router;
  - 0 otherwise.

  Each stream group keeps the domain and route parsed from its name.
- **`open()` with no valid stream:**
  - `-ENODEV` or `-ENXIO` at once, blocking or not;
  - with the XDomain present: `-EAGAIN` for `O_NONBLOCK`;
  - otherwise an interruptible wait for the stream service, re-checking the
    XDomain at least once a second, so an unplug or a domain removal ends it
    within about 1 s. The wait ends with `-ETIMEDOUT` after the timeout,
    `-ERESTARTSYS` on a signal, and `-ENXIO` on rmdir.
  - Timeout: `thunderbolt_stream.open_timeout_ms` (default 30000, capped at
    600000), overridable per endpoint with configfs `open_timeout_ms`
    (-1 follows the parameter, 0 does not wait).
- **Behaviour change for users:**
  - `O_NONBLOCK` without a stream used to return `ENXIO` and now returns
    `EAGAIN` if the XDomain is there.
  - Opening before the cable or the XDomain exists now fails with `ENXIO`
    instead of waiting. Clients retry, with backoff or by polling with
    `O_NONBLOCK`.

**Tests.**

- `tests/stream_open_cases.h` (actual open path):
  - no domain and no XDomain, blocking and non-blocking;
  - `EAGAIN`;
  - the bounded wait;
  - the peer appearing during the wait;
  - the XDomain, the domain or the item (rmdir) going away during the wait;
  - a signal;
  - the timeout settings and the cap.
- `tests/xdomain_presence_cases.h`: the actual `tb_xdomain_presence()` against
  a bus model (missing domain, missing XDomain, a router with the same name,
  unplugging, hexadecimal routes, references balanced).
- Each safeguard was reverted once (mutation check); every reversion fails at
  least one test.
