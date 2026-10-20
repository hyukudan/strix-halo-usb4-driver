# GPU completion path: timing, polling and CPU latency (0040–0042)

Status (2026-10-20): offline-tested and compile-tested only. Nothing here has
run on hardware yet. Every option is off by default, so with no settings
changed a kernel with 0040–0042 completes GPU and host streams the way
0001–0039 do. The only addition is the counters.

## The problem

Back to back, a GPU-direct exchange completes in 7–17 us. After a compute
gap of a few milliseconds, the same exchange takes hundreds of
microseconds: about 287 us more per exchange on the two-node runs of
2026-10-20. Host streams show the same effect to a lesser degree.

On 0001–0039, a GPU ring completes like this:

1. The controller writes back a descriptor.
2. The MSI-X interrupt fires.
3. `__ring_interrupt` queues `ring_work` on the unbound `tb_dma` workqueue.
4. The worker reaps the descriptors and runs the callbacks.
5. The last callback of an operation validates the 0016 terminal, moves the
   operation to FINISHING, signals the fence and publishes the EVENT.

GPU rings never poll: 0010's NAPI path is for host RX rings only. After an
idle gap, both the interrupt and the worker can wait for a CPU to leave a
deep idle state and for the scheduler. Interrupt moderation is only 8 us,
so it is not the main cost. These patches measure where the time goes and
offer two opt-in ways around the slow part.

## 0040: completion-path timing

The ring core stamps three times per collection, at no cost to the
interrupt path:

- the last interrupt (`last_irq_ns`);
- the worker's start;
- the moment the batch was reaped.

It also counts:

- interrupts;
- collections (batches reaped by the worker or by `tb_ring_collect()`);
- their mean and maximum duration.

A canceled batch (stop) is not counted as a collection.

GPU operations add their own times:

- the SUBMIT;
- the doorbell (just before the descriptors are enqueued);
- the ring stamps of their last frame, copied by the callback;
- the callback;
- the fence (FINISHING);
- the EVENT that returns the operation's last batch.

They are accounted per direction when EVENT returns a completed operation.
Failed and canceled operations are not accounted.

Read it with
`cat /sys/kernel/config/thunderbolt/stream/<service>/<name>/gpu_timing`.
Write anything to it to reset it. Without a GPU session, reading returns
`none` and writing fails with `ENXIO`. The read-out fits in one page:

```
mode irq polls 0 polled_frames 0 qos_us -1
ring tx irqs N collections N collect_ns_mean N collect_ns_max N
ring rx ...
tx ops N
tx <stage> <count> <mean_ns> <max_ns> <16 histogram buckets>
...
tx last seq S slot K ns <7 offsets from SUBMIT, -1 = not seen>
rx ...
```

| Stage | From → to | What it shows |
|---|---|---|
| `doorbell` | SUBMIT → doorbell | Driver submission cost. |
| `reap` | doorbell → last frame reaped | Wire time + interrupt + worker (IRQ mode), or + time until the next EVENT/fallback (poll mode). The first point at which software sees COMPLETED. |
| `irq_work` | interrupt → worker start | `tb_dma` queueing and wake-up latency. Only in IRQ mode; an interrupt older than the doorbell is ignored. |
| `callback` | reap → callback of the last frame | Earlier frames' callbacks in the same batch. |
| `fence` | callback → fence signalled | 0016 terminal validation and FINISHING. |
| `event` | fence → EVENT returns it | How long user space took to ask. |
| `total` | SUBMIT → EVENT | The operation as user space sees it. |

The histogram buckets are <1 us, then [2^(k-1), 2^k) us, and the last
bucket is open-ended (≥16 ms). The `last` lines keep the four most recent
operations per direction with every stamp, so one slow exchange can be
traced end to end.

## 0041: opt-in polled completion

| Setting | Where | Default |
|---|---|---|
| `gpu_poll` | configfs, per endpoint: -1 follows the module parameter, 0 interrupts, 1 polling | -1 |
| `thunderbolt_stream.gpu_poll` | module parameter (bool) | off |
| `gpu_poll_budget` | module parameter: frames per ring per collection, 1–4096 | 64 |
| `gpu_poll_fallback_ms` | module parameter: watchdog period while polling, 1–100 ms | 1 |

The mode is chosen when the GPU session opens. With polling, both GPU
rings are allocated with `RING_FLAG_NO_INTERRUPT`, so no interrupt and no
`tb_dma` worker run for them. Completions are collected instead by:

- **`SGU_IOC_EVENT`**, before it scans for events. It only tries the DMA
  submission lock: if SUBMIT, the watchdog or STOP holds it, EVENT collects
  nothing and returns what is already published, without sleeping.
- **The watchdog**, every `gpu_poll_fallback_ms` while operations are
  pending. This covers callers that wait in `poll()`/`select()` or on the
  fence and never call EVENT.

A collection calls the new core function `tb_ring_collect()` on the TX
ring, then the RX ring. It takes completed descriptors in ring order, up to
the budget, and refills the hardware ring with pending descriptors
(`ring_write_descriptors`). It then runs the same callbacks that
`ring_work` would. The 0016 chain is unchanged:

- terminal validation;
- callbacks exactly once;
- FINISHING → fence → EVENT;
- STOP cancels what was not collected, exactly once, and nothing is
  collected after a stop or during close.

`tb_ring_collect()` refuses rings that have interrupts (it warns once and
returns 0).

To see which mode a session runs in, read the first line of `gpu_timing`
(`mode poll`). It also shows how many collections ran and how many frames
they took.

## 0042: opt-in CPU latency QoS per session

| Setting | Where | Default |
|---|---|---|
| `latency_qos_us` | configfs, per endpoint: -1 follows the module parameter, otherwise 0–100000 us | -1 |
| `thunderbolt_stream.latency_qos_us` | module parameter: -1 none, otherwise clamped to 100000 | -1 |

When a limit is set, the endpoint holds a `cpu_latency_qos` request with
that limit while its rings run. Then a CPU does not enter idle states whose
exit latency is above the limit. 0 keeps every CPU out of deep idle, like
holding `/dev/cpu_dma_latency` at 0, but only while the stream runs.

- **Host streams:** taken when `open()` starts the rings. Released when the
  last close stops them, and also on suspend, detach or a failed start.
  Resume takes it again.
- **GPU sessions:** taken by ARM. Released by STOP, by close, by a watchdog
  failure, by a failed ARM, by DMA revocation and by domain abandonment
  (which stops nothing else).
- At most one request per endpoint. Every release path is idempotent.

The request is global (all CPUs). It costs idle power while the stream is
open, so it is meant for A/B runs and for sessions that exchange often.
`gpu_timing` shows the limit a GPU session holds (`qos_us`). For a host
stream, `/dev/cpu_dma_latency` shows the system-wide value.

## A/B recipe

Run each arm on the same workload, with the session closed while settings
change. Reset `gpu_timing` at the start of each arm and read it at the end.

1. Baseline: everything at default (`mode irq`, `qos_us -1`).
2. `latency_qos_us=0` on the GPU endpoints: tests whether idle-state exit
   latency is the cost.
3. `gpu_poll=1`: removes the interrupt and the worker.
4. Both.

No-patch checks on the baseline (root):

- `throttling=0` on the endpoints, set while the session is closed;
- `/dev/cpu_dma_latency` held open at 0 for one run.

In IRQ mode, compare `irq_work` and `reap` before and after. If `irq_work`
falls with the QoS, idle exit was the cost. If `reap` stays high while
`irq_work` is small, the time is before the interrupt.
