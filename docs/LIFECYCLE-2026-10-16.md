# Detached quarantine and property-notification lifecycle

Development follow-up to driver PR #2 at
`e990c7c3dc1955c6c39a18c8dbbd359ad9a01b79`. Additive kernel patch 0008;
no rewrite of the immutable capture, old patches, workflows or userspace ABI.
The queued-engine package remains a separate delivery; this patch does not
publish it or activate a production engine backend.

## 1. Prove path removal before releasing detached resources

`tb_xdomain_disable_paths()` can return success for an unplugged peer while the
software connection manager still has its DMA tunnel in the inventory. The
pinned `tb_disconnect_xdomain_paths()` skips synchronous teardown in that case.
That acknowledgment is not sufficient to free or recycle the old NHI ring.

The new `tb_domain_dma_paths_busy()` asks the connection manager about actual
DMA tunnel ownership, under `tb->lock`. Positive TX/RX ring IDs are matched
independently: a remaining RX-only or TX-only tunnel prevents cleanup. Both
arguments -1 query all DMA tunnels. Return 0 means an affirmative empty matching
inventory, 1 means busy and a negative value means no proof. Unsupported
connection managers return `-EOPNOTSUPP`. A busy domain mutex returns `-EAGAIN`;
this query never recursively waits for the same lock during removal.

This is a software path-inventory proof, **not** proof of DMA, PCI transactions
or GPU completion. Reclamation additionally requires the existing stopped-ring,
drained-callback, completed-fence and unarmed-session conditions. A timeout or
successful disconnect acknowledgment cannot substitute for those conditions.

## 2. Preserve the original owner across detach/reconnect

The GPU context retains its original XDomain reference and the immutable HopIDs
captured in its identity. STOP now uses these, not whatever endpoint and IDs a
later connection placed into `sdev->stream`. On detachment, unresolved original
HopID ownership transfers to that retained context. Successful cleanup releases
those IDs once in the original allocator. Detaching a replacement stream cannot
transfer its IDs to the old context or cause the old cleanup to release them.
ConfigFS already rejects reconfiguration while a GPU context remains, including
quarantined contexts; that existing protection is preserved.

Administrative `gpu_recover = reap` can now reclaim a **peer/cable-detached**
quarantine when the original local host controller remains bound and the actual
old paths and ring/callback ownership have gone. It does not require the old
service to still be the currently attached stream. It requires no open users and
administrative authority, as before.

The reaper takes an independent reference to the original DMA device and uses
`device_trylock()` to stabilize its driver binding. An unbound/replaced device,
driver-data mismatch, shutdown/going-away NHI or busy device lock rejects the
operation before touching mappings or controller state. A device reference alone
does not preserve driver-managed MMIO/devres after unbind. The temporary reference
also survives successful cleanup, which destroys the retained context.

`reset` remains stricter than `reap`: it requires the original current live
endpoint. It cannot reset a newly connected peer on behalf of an old quarantine.
Cleanup does not recreate an application session; the application still needs a
fresh peer-agreed epoch and reimport.

## 3. Strengthen exclusive reset against all tracked DMA consumers

Before the existing NHI ring-allocation gate, the reset backend now requires an
empty connection-manager DMA tunnel inventory. This includes tunnels with idle
rings and tunnels awaiting deferred hotplug removal. It propagates an unavailable
inventory as failure, rather than silently treating it as empty. The domain lock
keeps new path creation out of this interval; the existing NHI gate keeps ring
allocation/start out of the reset itself.

The controller-wide policy remains **exclusive and administrative**. No shared
consumer is stopped, and USB4NET/other streams retaining rings or paths veto the
reset. This is not transparent all-consumer recovery, automatic peer replay, a
PCI function reset or electrical lane retraining.

## 4. Notify after enumeration without creating a reciprocal loop

The snapshot had removed the notification following completed enumeration.
Patch 0008 restores that notification **once per handshake**, not after every
subsequent read of the peer's properties. Genuine local service-property changes
still request their own notification.

All notification bookkeeping is protected by `xd->lock`. A monotonic local
change serial prevents a completed older request, whether successful or failed,
from consuming the pending state/retry budget of a later update. Notifications
coalesce without postponing work already queued. Each genuine update has the
existing bounded initial attempt plus ten retries; errors no longer decrement
an exhausted retry counter indefinitely. Failure is logged on exhaustion rather
than on every retry.

Suspend/removal closes notification admission before canceling queued work.
Updates cannot requeue it behind cancellation. A new INIT resets that gate and
the once-per-handshake flag. Resetting is deliberately done in the INIT worker,
not in `start_handshake()`, which can already be called under `xd->lock`.
No property-response wire format, lane-bonding state machine or link-rate policy
is changed by this notification fix.

## Reconstruction and local use

Two upstream connection-manager files are absent from the original capture:
`drivers/thunderbolt/tb.c` and `drivers/thunderbolt/tb.h`. Their Git blob IDs were
read from the pinned revision and recorded in `provenance/lifecycle-base.json`.
The overlay builder requires these exact bytes from `--kernel-source` or
`USB4_KERNEL_SOURCE`, validates both before copying either and never downloads
or modifies the supplied kernel tree. Wrong hashes, symlinks, duplicate paths,
traversal and attempts to overwrite captured files are rejected.

```sh
python3 tools/prepare_overlay.py audited-overlay \
  --kernel-source /path/to/pristine/pinned-linux
USB4_SOURCE_TREE="$PWD/audited-overlay" \
  USB4_KERNEL_SOURCE=/path/to/pristine/pinned-linux \
  python3 -m unittest discover -s tests -v
```

The supplied kernel-source files must still be the pristine pinned versions,
not files to which this series has already been applied. A full kernel checkout
can instead receive every entry in `patches/series`, in order. Internal public
and CM structure layouts change; rebuild all compatible kernel/module consumers.
Do not mix these objects with old Thunderbolt/USB4NET modules. The old workflow
was not altered; a future manual workflow invocation must provide these new
external inputs before using the overlay materializer.

The package's APPLY.py validates the exact driver-repository base and patch
inputs, applies the repository diff in an isolated temporary worktree, rebuilds
the entire overlay and runs its tests before changing the user's checkout. A
failed real reconstruction or regression leaves that checkout unchanged.
It neither commits nor pushes, installs/loads modules, resets hardware, changes
ConfigFS nor invokes Actions.

After deliberate native build/install/boot and peer quiescence, the existing
ConfigFS `gpu_quarantine` and `gpu_recover` interface is used. Write `reap` only
to the known retained stream's `gpu_recover` attribute, with no open users.
`-EIO` means the stop/path-release proof is still insufficient; `-EAGAIN` can
mean the controller binding is busy. Neither authorizes bypassing the checks.
Do not remove arbitrary ConfigFS groups or repeatedly force-reset a shared NHI.

## Validation and explicit remaining scope

The delivered sources pass 61 portable tests with each of GCC and Clang. These
include the 33 prior tests and 28 new cases: four detached-ownership/device
lifetime cases, two reset-inventory cases, two actual inventory-callback cases,
seven notification lifecycle cases, seven external-base validation cases and
two bundle-only patch/context roundtrips and four applicator-guard/outer-patch tests. The C harnesses use ASan/UBSan. The
upstream tunnel-matching implementation used in the inventory test is attributed
to its exact Linux source; list/kernel/transport APIs remain explicit doubles.

Three compile-only checks pass against installed Linux 6.12.107 headers with
W=1 and -Werror. They cover the complete GPU and reset helper sources and the new
notification helper. Private CM/stream declarations, newer PCI IDs and the
relocated service-ID header are explicit compatibility substitutions. This is
not a Linux 7.3-rc5 build or module link. No probe object is deployed.

The two patch tests apply and reverse the actual new patch on complete retained
GPU/helper files plus clearly labeled excerpts of other source files. They do
**not** establish complete upstream reconstruction. A full pinned kernel tree
was not present and could not be downloaded in this environment; the real
full-series check is enforced by APPLY on the operator's checkout. No hardware,
unplug/suspend execution, native GPU visibility result or bandwidth measurement
was obtained here.

Still open: actual PCI/NHI disappearance and unbind while retained resources
exist (the pinned PCI remove path waits for domain release), transparent recovery
of cooperating and non-cooperating consumers, physical GPU/cache validation,
model-cluster qualification and the failing link's physical negotiation. It is
not safe to manufacture quiescence merely to clear those entries. No IOMMU
disable or unconditional global-reset workaround is introduced.

## Primary source provenance

- Linux `72d3fcf802c45d00b300f25b848a93c3a2bd7c7e`,
  `drivers/thunderbolt/{tb.c,tb.h,tunnel.c}`, fetched through GitHub.
- Driver PR #2 `e990c7c3dc1955c6c39a18c8dbbd359ad9a01b79`, captured
  `stream.c`, `xdomain.c`, `pci.c`, plus materialized recovery/dependency sources.
- The post-enumeration notification motivation was also checked against Milo
  Chen's June 24, 2026 Linux USB patch discussion. The bounded/coalesced lifecycle
  here is not a claim that the unconditional upstream one-line change was safe
  for the captured engine/peer behavior without modification.
