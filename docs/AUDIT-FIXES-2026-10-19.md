# External audit fixes 0020–0035 (2026-10-19)

State: implemented and tested **offline only** (unit tests with sanitizers,
`W=1` object builds, and full `W=1 vmlinux modules` builds with modpost on
cluster1 with `USB4_GPU_STREAM=y` and `=n`). Nothing here has run on the Strix
Halo nodes yet. The hardware checks at the end must pass before the series
becomes the installed kernel.

Source: the external audit of 2026-10-19: section A (kernel lifecycle,
A1–A12), section D (hardware matrix), and the addenda E (Gen3 follow-up,
E1–E3) and F (refined A12 model), against PR #1 head `dfc27c5` (0001–0019).
Line references in the verdicts are to that tree.

## Numbering

The audit fixes are **0020–0035**, directly after 0019. The per-stream
end-to-end setting on `perf/stream-throughput` (also numbered 0020 there) is a
measurement-only option and moves to the end: it gets the next free number when
that branch is rebased on this one. The other choice, putting these fixes after
the perf patch, would make correctness fixes depend on an experimental patch.
The perf patch touches `stream.c` too, so its context is refreshed during that
rebase.

## Verdicts

| # | Verdict | Where (dfc27c5) | Patch |
|---|---------|-----------------|-------|
| A1 | Confirmed | `nhi.c:1194-1214` `nhi_shutdown()`, `nhi.c:512` `ring_msix()` | 0020 |
| A2 | Confirmed | `stream.c:519` `tbstream_dev_valid()`; unlocked at `:976`, `:1103` and in wait conditions `:1022`, `:1145`, `:1258` | 0021 |
| A3 | Confirmed | `stream.c:2386-2415` `tbstream_resume()` | 0022 |
| A4 | Confirmed | `stream.c:605-645` `tbstream_dev_alloc_rx_buffers()` | 0023 |
| A5 | Confirmed | `dma_recovery.c:447-462` `tb_dma_revoke_bus_master()` | 0024 |
| A6 | Confirmed | `stream.c:546-565` `tbstream_dev_rx_callback()`, read at `:962` | 0025 |
| A7 | Confirmed | `stream.c:759` `tbstream_dev_send_close()`, comment at `:1369` | 0026 |
| A8 | Confirmed | `stream.c:962` read, `:1089` write | 0027 |
| A9 | Confirmed for the device mutex | `gpu_stream.c:1807` | 0031 |
| A10 | Confirmed | `stream.c:923-925` `tbstream_dev_stop()` | 0028 |
| A11 | Confirmed | `stream.c:330` `module_param(spin_us)`, limit `:1484` | 0030 |
| A12 | Confirmed, also in the field (addendum F) | `dma_recovery.c:318-352` `tb_domain_teardown_reset_work()` | 0029 |
| E1 | Confirmed | `xdomain.c:1811-1813` (`XDOMAIN_STATE_BONDING_UUID_LOW` in `tb_xdomain_state_work()`) | 0033 |
| E2 | Confirmed | `tb.c:3114` (`tb_start()`; the resume path at `:3221` is non-USB4 only) | 0034 |
| E3 | Confirmed (no recovery existed); opt-in policy added | `xdomain.c:1434` (`tb_xdomain_link_state_change()`), enumeration at `:1536` | 0035 |

**A1.** An abandoned GPU context keeps its rings, which is the point of the
quarantine. `nhi_shutdown()` masks the interrupts but leaves those rings in
`tx_rings[]`/`rx_rings[]`, with their MSI-X vector still requested and their
work still queueable. `ring_msix()` reads `ring->nhi` before anything else.
After remove the NHI, its MMIO and the workqueue are freed, so a late vector or
work item would use freed memory. **Fix (0020):** after
`nhi_disable_interrupts()`, `nhi_shutdown()` clears each remaining slot under
`nhi->lock`, then frees the ring's vector (`free_irq()` waits for a running
handler) and cancels its work. The ring memory and mappings stay, as the
quarantine requires; only the CPU paths end.

**A2.** `tbstream_dev_valid()` reads `sdev->stream` and dereferences its
service. read/write called it before taking `sdev->lock`, and the wait
conditions called it without any lock, while detach clears the pointer and
drops the stream. **Fix (0021):** `tbstream_dev_valid()` asserts the device
mutex (lockdep). The unlocked pre-checks are gone. Wait conditions use
`tbstream_dev_attached()`, which tests the pointer without dereferencing it.
Attach and detach publish the pointer with `WRITE_ONCE()`. Every woken waiter
re-validates under the lock.

**A3.** A failed `tbstream_dev_start()` in resume returned at once: the open
file kept `users != 0` with NULL rings (busy_poll then reaches
`tb_ring_poll(NULL)`), the other modes waited forever, and the later endpoints
were never resumed. **Fix (0022):** suspend and resume walk the group under its
lock, and resume takes each device's mutex. A device that fails to start gets
the existing terminal state, `detached`: its files fail until closed, then a
new open starts afresh. Its waiters get `EPOLLHUP | EPOLLERR`, a ratelimited
warning names it, the other devices still resume, and the first error is
returned.

**A4.** RX buffers were posted on a running ring and the counters were zeroed
afterwards, so a completion during posting was lost. **Fix (0023):** the
counters are zeroed before the first post.

**A5.** The revocation proof discarded `ioread32(REG_CAPS)`, so an all-ones
read from a dead device passed. It also assumed posted writes are not
reordered, which Relaxed Ordering would break. **Fix (0024):** the proof
fails (`-EPROTO`) if `PCI_EXP_DEVCTL` cannot be read or has
`RELAX_EN` set. It fails (`-EIO`) if REG_CAPS reads `~0` or has no hop
count. Without the proof the context stays quarantined.

**A6.** A frame with a CRC error or buffer overrun was counted and its contents
returned as data; the callback only logged it. **Fix (0025):** the callback
wakes readers with `EPOLLERR`. Read stops at the damaged frame and returns the
bytes before it, then `-EIO`. The stream stays failed (`rx_failed`, `EPOLLERR`
in poll) until the next start, i.e. a new open. Silently dropping part of a
byte stream is not acceptable.

**A7.** In interrupt mode CLOSE was reserved once; with the TX ring full it was
dropped and the peer saw no EOF. The comment said "twice". **Fix (0026):** wait
up to `TBSTREAM_DEV_CLOSE_WAIT_MS` (500 ms) for a TX slot. If none frees, a
ratelimited message says the peer sees no EOF. The comment now matches.

**A8.** read/write could return an error after moving bytes. A partial copy
left the iterator ahead of the accounting. A zero-length read could block.
**Fix (0027):** empty I/O returns 0 at once. A partial copy is reverted
(`iov_iter_revert()`) to what was accounted. A failed `tb_ring_tx()` reverts
the frame and its reservation. Both paths return the bytes moved if any,
otherwise the error. `-EFAULT` is returned only with no progress.

**A9.** `SGU_CAP_NONBLOCK_SUBMIT` promised `EAGAIN` instead of waiting, but the
ioctl slept on the device mutex first. The other locks in submit were already
bounded: the reservation lock is only tried (`dma_resv_trylock`), and the
context locks are short spinlocks. **Fix (0031):** for SUBMIT and RX_REPOST
with `O_NONBLOCK`, the device mutex is tried and `-EAGAIN` returned if it is
taken. Other commands keep blocking semantics. The UAPI comment now states the
scope: the device mutex and dependency waits, not the short internal locks.

**A10.** Stop flushed RX with a 500 ms timeout, but idle posted RX buffers
never complete, so every close in interrupt mode took the full timeout. The
mock flush returned at once. **Fix (0028):** stop drains TX as before and no
longer flushes RX. `tb_ring_stop()` cancels the posted buffers and
synchronizes with the callback before the rings are freed, so stop-before-free
still holds.

**A11.** configfs `spin_us` is capped at 1000 us, the module parameter was
not, and a reader busy-polls with `rx_lock` held. **Fix (0030):** a setter
rejects values above `TBSTREAM_DEV_MAX_SPIN_US`, and start caps the value it
reads.

**A12 (with addendum F).** The 0018 reset was dropped after 10 attempts
(about 5 s) while another user held the NHI. The field logs of both nodes
(21:02–21:16) show the case:
- At the start of a run, a path teardown met that run's own streams
  ("NHI in use after 10 attempts (2 TX and 2 RX rings)") and the reset was
  dropped.
- It only happened about two minutes later, because the last stream's close
  tore its paths down again: a stream close disconnects its paths first and
  frees its rings right after.
- A user that leaves *without* a path teardown (GPU context, USB4NET, a late
  `tb_ring_free()`) left the NHI without the recovery.

**Fix (0029), the pending model of addendum F:**
- The reset is a state per NHI (`teardown_reset_pending`, under `nhi->lock`).
- A path teardown sets it and starts a fresh budget of fast attempts. When
  they run out with consumers present, it parks with no polling and logs
  `host interface reset after DMA teardown pending: NHI in use (N TX and M RX
  rings ...)`.
- Two events re-arm it: freeing the **last** data ring (`tb_ring_free()` →
  `tb_nhi_ring_freed()`) and the next path teardown.
- The admission under the domain lock re-checks, right before the reset, that
  no ring, DMA tunnel or other reset uses the NHI. The veto stays, and there
  is no longer budget.
- Domain removal drops it (`... dropped: the domain is being removed`), also
  when service drivers free their rings during the removal.
- Suspend and freeze keep it; `tb_domain_complete()` re-arms it.
- The existing "reset after DMA teardown (attempt N)" and "... failed" lines
  are the only ones that clear it.

The audit's second point is right: the 30 s domain timeout bounds only the
final reference wait, not every earlier mutex or callback. That is documented,
not changed.

**E1.** In `XDOMAIN_STATE_BONDING_UUID_LOW`, the lower-UUID side called
`tb_xdomain_lane_bonding_enable()` and ignored its result. **Fix (0033):**
- The result is checked by `tb_xdomain_bond_lanes_uuid_low()`.
- Failures are retried at most `XDOMAIN_BONDING_RETRIES` (2) times, with a log
  line each. Each attempt may wait 10 s for the width, so the bound matters.
- Then the failure is reported like the other bonding states (speed and
  width), and enumeration continues unbonded.
- `-ENODEV` (no second lane adapter) is final. An `-EAGAIN` from below cannot
  loop the state machine.

**E2.** `tb_start()` ignored the result of `tb_switch_reset()` on a USB4 v1
host router and went on as if the boot firmware's tunnels were gone. **Fix
(0034):**
- The decision moves into `tb_start_reset_host()`.
- A failed reset is logged with its errno.
- The CM then falls back to discovering the boot firmware configuration, the
  same path as when no reset is requested.
- USB4 v2 and non-USB4 hosts are unchanged.

**E3.** Nothing retried a link that trained at Gen2 while both ends can do Gen3
(field: a real Gen2 x2 link, CS_0 supported `0xc`, target `0xc`). **Fix (0035),
opt-in:** `thunderbolt.gen3_retrain=N`, default 0 (off), at most 3.

When it runs:
- For a **new** XDomain connection, before its properties are read. No service
  or stream of it exists yet.
- The link must be below Gen3.

Conditions, all required, each logged when it blocks:
1. The local lane adapter supports Gen3 and targets it (LANE_ADP_CS_0/1).
2. The local port advertises **Gen3 Support in LINK_CONF** (sideband register
   0x0C, byte 1 bit 5). This is read first. Without the bit (the `d3` captures
   of cluster2) the limit is the port or its on-board retimers: that is logged
   and nothing is reset.
3. No DMA tunnel in the domain and no data ring on the NHI. This is checked
   under the domain lock, which is only tried: the CM may be removing this
   XDomain under it.

What it does:
- A USB4 downstream port reset (`usb4_port_reset()`), then the handshake starts
  over. If the reset causes an unplug, that unplug removes the XDomain.
- At most N resets per port until it trains at Gen3, then one warning and the
  Gen2 link is kept. Reaching Gen3 is logged and restarts the budget.
- Both nodes decide on their own. With N=1 each tries at most once.

**0032** adds the kernel-doc lines that `W=1` reported for `struct
tbstream_dev` (members from 0010/0011/0015) and `struct tbnet`
(`recovery_node`, 0009). It changes no code. It is a separate patch so the
already published patches keep their content.

## Tests

Each fix has offline tests that run the actual patched function in the
existing harnesses, under AddressSanitizer and UndefinedBehaviorSanitizer:

| Patch | Test (module: case) |
|-------|---------------------|
| 0020 | `test_nhi_shutdown`: abandoned rings, leaked rings, clean shutdown |
| 0021 | `test_stream_host_path: stream_deref_under_lock`. The harness fails any service dereference without the device mutex |
| 0022 | `resume_failure_fails_open_files` (resume failure mock) |
| 0023 | `early_rx_completion_is_kept` (early RX completion mock) |
| 0024 | `test_dma_recovery: teardown_revoke` (Relaxed Ordering, DEVCTL read failure, all-ones REG_CAPS; the zero hop count branch has no test of its own) |
| 0025 | `damaged_rx_frame_fails_stream` (CRC error and overrun, polled and interrupt mode) |
| 0026 | `close_waits_for_tx_slot` |
| 0027 | `partial_io_is_accounted` (copy faults, TX failure, zero-length I/O) |
| 0028 | `close_does_not_flush_rx`: the `tb_ring_flush()` mock now waits like the real one and does not complete idle RX |
| 0029 | `test_dma_recovery`: `teardown_reset_pending_until_owner_leaves`, `teardown_reset_rechecks_before_reset`, `teardown_reset_pending_at_removal`, `teardown_reset_pending_across_suspend` (the acceptance cases of addendum F), plus the existing teardown cases updated. `test_domain_teardown` checks the `tb_ring_free()`, remove, suspend/freeze and `tb_domain_complete()` wiring |
| 0030 | `spin_us_parameter_is_bounded` |
| 0031 | `test_gpu_queue: nonblock_contended_mutex` |
| 0033 | `test_cm_link: bonding_retried_and_bounded`, state machine wiring |
| 0034 | `test_cm_link: host_reset_result_checked`, `tb_start()` wiring |
| 0035 | `test_cm_link: gen3_retrain_policy` (registers and sideband scripted, including the `f3`/`d3` LINK_CONF values), wiring |

**Removal check.** Each safeguard was reverted on its own, 55 mutations in
total, and every one fails at least one test:

| Fix | Mutations |
|-----|-----------|
| A1 | 3 |
| A2 | 4 |
| A3 | 2 |
| A4 | 1 |
| A5 | 2 |
| A6 | 4 |
| A7 | 2 |
| A8 | 6 |
| A10 | 1 |
| A12 (F) | 9 |
| A11 | 2 |
| A9 | 2 |
| E1 | 5 |
| E2 | 2 |
| E3 | 10 |

Each patch commit also passes the whole suite on its own (`unittest discover`
against the tree at that patch).

## What still needs hardware (audit section D)

None of these fixes has run on the nodes. Before the series is installed:

1. Unplug one cable under RAM and GPU traffic while the other cable keeps its
   traffic (0020–0022, 0029).
2. Close, service removal and configfs `rmdir` with open files (0021, 0026,
   0028). Check close latency in interrupt mode: it should no longer take
   ~500 ms.
3. An alloc/start failure during resume, e.g. by exhausting the hop budget
   before suspend (0022): the file fails with `EPOLLHUP|EPOLLERR`, the other
   endpoints resume, and dmesg shows "resume failed".
4. Removal with a retained or abandoned GPU context (0020, 0024): no late
   interrupt or work after remove. On both NHIs, also check the dmesg line
   for the revoke result (Relaxed Ordering is expected to be off).
5. Open with no peer, a late peer, and a peer that disappears (0019).
6. TX full during close (0026), CRC/overrun (0025, needs a marginal cable or
   error injection), and early RX (0023).
7. Reboot and module unload with live XDomains and streams.
8. A pending teardown reset (0029). Two streams on one AMD NHI: tear one
   down and keep the other open for more than 5 s; dmesg shows "pending: NHI
   in use". Close it: the reset follows. Also with a GPU context or USB4NET as
   the last user, freed without a path teardown.
9. `O_NONBLOCK` SUBMIT under contention (0031) in the GPU runner.
10. A KASAN/KCSAN/lockdep kernel on the concurrent paths. lockdep now checks
    the 0021 invariant.
11. Lane bonding (0033): the new lines appear only on a bonding failure. A
    normal connection logs nothing new.
12. Boot (0034): no "host router reset failed" line expected.
13. Gen3 retrain (0035), only without streams. With `gen3_retrain=1` set
    before the lab service creates endpoints, reconnect in the mixed 40+20
    state. dmesg names the decision: retrained, "does not advertise Gen3
    Support (LINK_CONF …)", or in use. Afterwards compare LANE_ADP_CS_1
    current speed and sysfs. Never with traffic.
