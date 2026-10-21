# GPU completion path: timing, polling, CPU latency and stalls (0040–0043)

Status (2026-10-20): offline-tested and compile-tested only. Nothing here has
run on hardware yet. Every option is off by default, so with no settings
changed a kernel with 0040–0042 completes GPU and host streams the way
0001–0039 do. The only addition is the counters.

Status of 0043 (2026-10-21): offline-tested and compile-tested only. It is
diagnostics only and changes no traffic; its counters are on by default
(`gpu_stall_ms` 10).

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

## 0043: TX/RX progress stall detector

### What was observed

Measured on 2026-10-21 on these AMD USB4 host interfaces, with GPU-direct
sessions (rings with `RING_FLAG_E2E`):

- **Observation.** TX descriptors sometimes went without completing for
  about 100–200 ms, and the matching RX descriptors on the peer did too.
- **Association.** These stalls were seen in runs where a TX could start
  while the peer's GPU RX ring had no posted descriptors, or had them only
  just re-posted. A GPU RX ring gets descriptors only per RX SUBMIT; host
  streams keep their RX ring always posted and showed no such stall.
- **Measured improvement.** Without any change in user space: 9–15 stalls
  per 3000 exchanges. With user space posting the next exchange's RX
  before the current TX: 0 stalls in 18 000 exchanges.
- **Mechanism: not demonstrated.** One explanation that fits is a TX
  waiting for end-to-end flow control credits from the peer until the
  controllers grant or resynchronize them. No register, trace or
  experiment has shown the credit state, so this remains a hypothesis.

0040's `gpu_timing` shows such a stall only as a long `doorbell_reap` stage.
That stage ends at the operation's *last* frame, so it does not show whether
the ring made no progress at all or merely progressed slowly. 0043 measures
the interval without progress itself.

### The rule for user space

**Post the receive of an exchange before the peer can transmit it.** In
practice: post exchange k+1's RX before your TX of exchange k. On these
controllers, a TX that started while the peer's GPU receive ring was empty
was observed to wait ~100–200 ms; that this is a wait for E2E credits is
the hypothesis above, not a demonstrated fact. Posting early is what
removed the stalls in the measurements.

### What it detects

Per GPU session and per direction: a submitted operation is pending (phase
PENDING, frames still on the ring) and no frame completed on that direction's
ring for more than `gpu_stall_ms`. The interval starts at the ring's last
completed frame, or at the SUBMIT of the oldest pending operation if that
is later (an idle ring is not a stall). A standby RX (posted by
`SGU_IOC_RX_REPOST` but not yet submitted) is not pending.

- **Progress** is recorded in the frame callback, the path that interrupt
  mode (`ring_work`) and polled mode (`tb_ring_collect()`) share, with the
  reap stamp of 0040.
- **Evaluation** happens at the collection points: the EVENT collection
  (polled mode) and the watchdog, which runs while operations are pending
  (every `gpu_poll_fallback_ms` polled, every 100 ms with interrupts) and
  collects before it checks deadlines. So it works in both modes, when TX is
  reaped lazily and when nothing calls EVENT. A completion or cancellation
  that ends an interval longer than the threshold that no collection point
  saw counts it instead (for example STOP 50 ms after a SUBMIT in interrupt
  mode, before the 100 ms watchdog ran): a cancellation never erases it.
- An **episode** is counted once, when it is first seen. It ends at the next
  completed frame on that ring, or when its frames are canceled (STOP,
  close, failure); a cancellation also ends the interval, so the remaining
  canceled frames do not count it again.
- **Diagnostics only.** Nothing is canceled, timed out, reset or re-armed
  because of an episode, and no deadline changes. The callbacks still run
  exactly once, the 0016 terminal check, FINISHING → fence → EVENT and STOP
  joining the collector are unchanged. The ring is read with the new core
  call `tb_ring_snapshot()` (under the ring lock, never under the session's
  spinlock), which changes nothing.

| Setting | Where | Default |
|---|---|---|
| `thunderbolt_stream.gpu_stall_ms` | module parameter: threshold in ms, 0 off | 10 |

An RX episode measures only that no RX frame was reaped for that long. It
does not show why: the peer may not have sent, or frames may have been
delivered but not completed or not collected. With exchange k+1's RX posted
early, waiting for the peer's next TX is normal and is counted too. Read RX
episodes together with the peer's TX episodes.

### How to read it

The lines come right after the `ring` lines of `gpu_timing`, before the
stage tables. Since 0043 the read-out holds whole lines only: a line that
does not fit the page is left out entirely. A heavy session (10^6
operations, 150 ms maxima, full records) fits with room to spare; only
synthetic 19-digit counters fill the page, and then the last lines of the
stage tables or records are left out, never these. The write that resets
`gpu_timing` also resets them. An episode still open at a reset is counted
again when a collection point sees it; the progress times are kept.

```
stall threshold_ms 10
stall tx episodes 3 total_ns 461203311 max_ns 171550120 open 0
stall tx last seq 812 slot 0 state ended duration_ns 171550120 seen_ns 10544210 pending_tx 1 pending_rx 1 head 37 tail 35 queued 0 front_completed 0 collect_gap_ns 1043211
stall rx episodes 0 total_ns 0 max_ns 0 open 0
```

(Example of the format, not a measurement.)

| Field | Meaning |
|---|---|
| `episodes`, `total_ns`, `max_ns` | Episodes counted; total and longest duration of those that ended. |
| `open` | 1 while an episode lasts. |
| `last seq`, `slot` | The oldest pending operation when the last episode was seen. |
| `state` | `open`, `ended` (a frame completed) or `canceled`. |
| `duration_ns` | From the last progress to its end (or to now, while open). |
| `seen_ns` | When it was first seen, from its start. Equal to `duration_ns` when only the ending completion saw it. |
| `pending_tx`, `pending_rx` | Pending submitted operations per direction when seen. |
| `head`, `tail`, `queued` | The ring when a collection point saw it: next descriptor to post, next to complete, frames waiting in the software queue for a free descriptor. -1 when no collection point saw it. |
| `front_completed` | 1: the controller had completed the descriptor at `tail` but nobody had collected it (a software delay). 0: the controller had not completed it. |
| `collect_gap_ns` | Polled mode: the longest interval between effective collections during the episode, including the time since the last one (while open, up to the read; when canceled, up to the cancellation). Small (about `gpu_poll_fallback_ms`) means the rings were looked at and made no progress; close to `duration_ns` means nobody looked. An EVENT that could not take the DMA lock is no collection. -1 in interrupt mode, where the ring's interrupt does the collecting. |

A TX episode with `front_completed 0`, `head` ahead of `tail`, in polled
mode a small `collect_gap_ns`, and on the peer an RX episode of about the
same length is compatible with a stall on the controller side. It is not
conclusive: it shows that software looked and found nothing completed, not
what the controller or the link was doing.

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
