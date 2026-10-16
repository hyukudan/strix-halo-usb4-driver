# External GPU dependencies and cluster readiness

Patch 0007 builds on the recovery series in PR #2. It does not modify the
original capture, internal wire sizes, ioctl numbers, or any Actions workflow.

## Correctness fix

A signalled dma_fence can have failed. Previously the submission dependency scan
used only dma_fence_is_signaled(). A failed GPU producer/reader could therefore
be treated as ready. The blocking retry could also lose the dependency error if
an exporter pruned that fence from its reservation object during the wait.

The implementation now checks dma_fence_get_status(), retains the observed
pending dependency across both blocking and nonblocking retries, and makes a
negative dependency status sticky for that operation. It publishes no new local
fence/descriptors for a failed dependency. STOP/RELEASE/close release the retained
reference. After dropping the device mutex it revalidates endpoint, import
identity and exact operation before recording the error; an old waiter cannot
poison a newly imported operation. Own non-overlapping slots retain their
independent timelines and are not misclassified as foreign dependencies.

A dependency failure is not repaired by retrying SUBMIT with the same identity.
Stop/release and explicitly recover the producer/session according to the
existing recovery contract. This is not an end-to-end GPU visibility proof.

## Local validation

The delivery's portable suite plus four new dependency regressions passed:
33 tests with GCC and 33 with Clang, using ASan/UBSan for the C API-double
harnesses. Cases cover failed completed dependencies in TX/RX, pruning during a
blocking wait, nonblocking pruning, STOP/RELEASE while waiting, and balanced
references. Test doubles are not the kernel, the GPU or a cache simulator.

The new patch's affected code was compared with the published PR #2 hunks.
The portable delivery is not a full pinned Linux checkout. A fresh reconstruction
of all seven patches on Linux 72d3fcf802c45d00b300f25b848a93c3a2bd7c7e, module
linkage and physical two-host validation are still required. Do not interpret
these tests as that build or as measured 40 Gb/s performance.

## Active outstanding work (supersedes historical single-operation audit text)

- Driver queues support up to eight slots per direction. The inspected native
  engine HIP/commissioning path still uses one operation per direction (paired
  TX/RX duplex is distinct from eight slots). Do not advertise engine queue-depth
  support merely because SGU_IOC_QUEUE exists in the driver.
- PR #2 provides explicit reset only for an exclusively idle supported AMD NHI,
  not transparent multi-consumer recovery. Unprovable/disconnected quarantines
  remain retained. No timer, fence error, or missing sysfs node authorizes free.
- GPU visibility requires testing the actual HIP export, DMA importer, raw RX
  GPU loads, copied execution loads and consumer-credit ordering. A new native
  GPU-side commissioning probe is being developed in the engine repository;
  neither that source nor a CPU/API-double pass qualifies the physical path.
- Property notifications remain unchanged until reproducible peer/discovery
  traces justify a minimal fix. Link training and property discovery are separate.
- Keep IOMMU enabled. Record both hosts, loaded driver identity and physical
  lane count/rate, and test unplug/suspend/new epochs with actual GPU work.

Primary API reference: Linux DMA-BUF documentation,
https://docs.kernel.org/driver-api/dma-buf.html (dma_fence_get_status and
reservation/fence lifetime). Future hardware qualification must remain separate
from software capability bits and from this PR's merge state.
