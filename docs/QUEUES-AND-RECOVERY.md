# GPU queues, synchronization and session recovery

Follow-up to the initial 2026-10-16 audit. Patch `0005` supersedes that report's
single-operation, GPU-wait-under-device-mutex and missing session-timeout notes.
The immutable capture and Linux `72d3fcf802c45d00b300f25b848a93c3a2bd7c7e`
base remain unchanged. Apply the complete series, not `overlay/` alone.

## Implemented contract

The default depth is one: all original ioctl numbers, structures and ABI identity
remain unchanged. Legacy RX_REPOST/RETIRE still retain RX ring ownership and
require identical slot geometry on reuse. New capability discovery is opt-in;
capability bits describe software interfaces, **not hardware qualification**.

`SGU_IOC_QUEUE` accepts the exact QUERY identity. Set `depth` to 1..8 before
IMPORT; zero queries the current depth. All output/reserved fields are zero on
input. Configuration cannot change while imported. TX and RX have the same
depth, up to eight independent operations each. Multi-operation import rejects
DMA mappings requiring cache synchronization: whole-map cache maintenance while
another slot is active is not safe. TX/RX reservation objects must be distinct.

Use `SGU_IOC_PREPARE_SLOT` with the existing operation fields and an explicit slot
0..depth-1. For ring size R and queue depth D, the payload bank is frames
0..R-D-1; terminal frame for slot S is R-D+S. Its byte offset is
`(R - D + S) * SGU_FRAME_BYTES`, relative to the imported window. The complete
4096-byte ranges of partially used payload frames are reserved. Payload ranges
cannot overlap any occupied operation or terminal bank. PREPARE never silently
chooses or replaces a slot. Handles remain globally increasing, sequence numbers
increase per direction, and submissions must follow increasing sequence order.
Both peers must agree the wire order and sizes before traffic starts.

Example for R=256 and D=4: payload frames 0..251 and terminal frames 252..255.
Four 63-frame payload regions fit. A submission includes its own terminal frame.
The payload itself is still opaque; the driver does not parse or invent a remote
acknowledgement protocol. Queue depth does not increase the number of hardware
rings or the total imported window size.

For D>1, consume every EVENT batch and RETIRE an operation before reusing its
slot, for either direction. Prepare/submit its replacement without stopping the
rings. RX_REPOST is rejected in this mode rather than silently placing old
frames ahead of another queued operation. For D=1 the original RX standby
contract is preserved, including refusal to change geometry unilaterally.

## Fences and parallelism

Each direction/slot has an independent fence timeline. A failed later enqueue
cannot signal an earlier slot that is still owned by the controller. Reservation
publication is atomic under the reservation lock and excludes only this
context's own non-overlapping slots from dependency waits. External producer and
consumer fences are still honored. A slot is not reused until its previous DMA
has completed and its event has been retired.

SUBMIT and RX_REPOST never wait for a GPU fence while holding the stream-device
mutex. A blocking request waits in slices, retaining only a fence reference
across unlock, then revalidates the current identity, generation, phase and
sequence. Opposite-direction requests, STOP and RELEASE can proceed meanwhile.
The dependency-wait budget is one second. O_NONBLOCK returns EAGAIN without
publishing a new fence or submitting any frame. EAGAIN requires a retry after
the producer becomes ready; this interface does not add EPOLLOUT readiness for
foreign GPU fences. Lock contention also returns EAGAIN in nonblocking mode.

`SGU_IOC_EXPORT_SYNC_FILE` exports one submitted operation's controller fence;
input fd is -1 and reserved fields are zero. Its descriptor is close-on-exec.
It remains valid after EVENT, RETIRE, RELEASE and stream close. Allocation/copy
failures undo the fd reservation. Each fence pins the driver module until its
last reservation/sync_file reference is gone; memory is freed through the RCU
fence helper.

**An operation fence is not a remote ACK, a payload-integrity check, or proof of
GPU cache visibility.** DMA-BUF implicit reservations cover the whole backing
object, not one region. Independent transport slots do not automatically allow
GPU compute on another region of that same object. Applications needing compute
and transfer overlap must preserve the exporter's synchronization rules, often
using separate backing objects/streams. Never discard other outstanding
reservations merely because one exported operation fence is signaled.

## Timeout and recovery behavior

The module parameters `gpu_dma_timeout_ms` (default 30000) and
`gpu_standby_timeout_ms` (default 300000) accept 100..3600000 milliseconds.
They are sampled on enqueue; lowering them does not alter existing deadlines.
The latter bounds physically queued legacy RX standby. A reused RX operation
receives a fresh active-operation deadline. This is a scheduled watchdog, not a
hard real-time guarantee: scheduling, a held ioctl mutex or user-memory faults
can delay execution. New submissions do not postpone an already queued watchdog.

On expiry or an explicit CANCEL the watchdog stops **this session's** rings and
paths, not the entire NHI. Outstanding frames report cancellation; expired
operations additionally report SGU_FRAME_ERROR_TIMEOUT and their fence reports
ETIMEDOUT after callbacks drain. Merely reaching a deadline never signals a
fence or authorizes unmapping memory that hardware might still use. A failed
stop retains mappings, callback sources and references. It can require hardware
recovery; it is not silently converted into success.

`SGU_IOC_RECOVER` uses the current imported identity with zero handle, sequence,
direction and output/reserved fields. It stops the session and succeeds only
once both rings are stopped, paths are removed and callbacks are drained. It
then releases imported windows, clears session failure and returns
SGU_RESULT_SESSION_RECOVERED with import_generation zero. Obtain the current
identity and IMPORT/ARM again using a **strictly newer peer-agreed epoch**. Old
identities are rejected. A disconnected device is not resurrected by RECOVER.
The peer must independently quiesce and agree the new session before traffic.

RECOVER is **not** a PCI reset, NHI reset or electrical link retrain. In
particular, this patch does not restore the removed unconditional AMD interface
reset after every stream close. A controller-wide reset must coordinate all
streams, USB4NET, control traffic and power-management users. Session recovery
cannot promise to repair the underlying AMD E2E-credit wedge or a 20 Gb/s link.

## Validation and remaining gates

The new tests compile the complete GPU extension's function bodies in a
userspace kernel-API simulator and execute eleven suites with AddressSanitizer,
UndefinedBehaviorSanitizer and leak detection. A separate test compares every
legacy ioctl number/size and key layout offsets to the immutable header.

The suites cover geometry/overlap/overflow, 16 simultaneous operations across
both directions, 2,048 operations with slot reuse, event pagination, legacy RX
reuse races, enqueue failure isolation, timeout with a stop that cannot be
proven, fresh-epoch recovery, real pthread blocking-wait interleavings, ordering,
nonblocking behavior, sync_file lifetime and fault rollback, module references,
noncoherent admission and watchdog deadline starvation. All twelve new tests
passed locally. The simulator does not model the kernel scheduler, MMIO, DMA,
cache coherence, PCI removal, real RCU grace periods or actual GPU execution.

An additional compile-only check of the complete implementation against local
Linux 6.12.107 headers passed with W=1 and -Werror. It used stand-ins for private
stream declarations and newer ring helpers; it was not a pinned-kernel build,
module link or hardware test. The existing CI remains responsible for applying
all five patches and compiling the affected Linux 7.3-rc5 objects with GPU mode
enabled/disabled. Consult its actual checks; a workflow definition is not proof
that the build ran or passed.

Before hardware deployment: complete the pinned build, boot a recoverable test
kernel, verify two-host payload integrity and GPU synchronization, stress
concurrent USB4NET/streams, and test cancellation, unplug, suspend/resume and
new-epoch reimport. Record both hosts' read-only link reports and throughput
before/after. No module installation, reload, host reboot, hardware reset or
40 Gb/s negotiation validation has been performed by this source change.

```sh
python3 tools/prepare_overlay.py audited-overlay
USB4_SOURCE_TREE="$PWD/audited-overlay" python3 -m unittest discover -s tests -v
python3 tools/usb4_diagnose.py --logs > "usb4-$(hostname).json"
```
