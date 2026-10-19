/* SPDX-License-Identifier: GPL-2.0-only */
/* Included after the extracted stream.c structures and functions. */
struct host {
    struct tb_nhi nhi;
    struct tb tb;
    struct tb_xdomain xd;
    struct tb_service svc;
    struct tbstream stream;
    struct tbstream_dev dev[2];
};

static void host_init(struct host *h, unsigned int ring_size)
{
    memset(h, 0, sizeof(*h));
    memset(tunnels, 0, sizeof(tunnels));
    h->tb.nhi = &h->nhi; h->xd.tb = &h->tb; h->svc.xd = &h->xd; h->stream.svc = &h->svc;
    for (int i = 0; i < 2; ++i) {
        struct tbstream_dev *s = &h->dev[i];

        s->stream = &h->stream;
        s->misc.name = i ? "tbstream3" : "tbstream1";
        s->in_hopid = 8 + 2 * i; s->out_hopid = 9 + 2 * i;
        s->ring_size = ring_size; s->throttling = 8192;
        s->cfg_rx_napi = -1; s->cfg_spin_us = -1; s->irq_cpu = -1;
        mutex_init(&s->lock); mutex_init(&s->rx_lock);
    }
}

static int start(struct tbstream_dev *s)
{
    int ret;

    mutex_lock(&s->lock);
    ret = tbstream_dev_start(s);
    mutex_unlock(&s->lock);
    return ret;
}

static void stop(struct tbstream_dev *s)
{
    mutex_lock(&s->lock);
    tbstream_dev_stop(s);
    mutex_unlock(&s->lock);
    assert(!s->tx_ring.ring && !s->rx_ring.ring);
}

static void assert_nothing_held(void)
{
    assert(!tunnel_count());
    assert(!atomic_load(&live_rings) && !atomic_load(&live_arrays));
    assert(!atomic_load(&live_pages) && !atomic_load(&live_mappings));
}

/* cluster1, 2026-10-19: an order-4 allocation failed during open() and every
 * later open of both streams on that NHI returned ENOMEM. Each failure point
 * must leave nothing behind, and the next open of either stream must work. */
static void open_failure_is_not_sticky(void)
{
    struct { const char *name; int *counter; int after; int enable_error; } points[] = {
        { "tx ring", &ring_fail_after, 0, 0 },
        { "tx frame array", &kvcalloc_fail_after, 0, 0 },
        { "tx page", &page_fail_after, 5, 0 },
        { "tx mapping", &map_fail_after, 7, 0 },
        { "rx ring", &ring_fail_after, 1, 0 },
        { "path enable", NULL, 0, -EIO },
        { "rx frame array", &kvcalloc_fail_after, 1, 0 },     /* the incident */
        { "rx page", &page_fail_after, 64 + 9, 0 },
        { "rx mapping", &map_fail_after, 64 + 3, 0 },
    };
    struct host h;

    for (unsigned int i = 0; i < sizeof(points) / sizeof(points[0]); ++i) {
        host_init(&h, 64);
        if (points[i].counter)
            *points[i].counter = points[i].after;
        enable_fail = points[i].enable_error;
        int ret = start(&h.dev[0]);
        if (!ret) {
            fprintf(stderr, "no failure injected at %s\n", points[i].name);
            abort();
        }
        if (points[i].counter && *points[i].counter != -1) {
            fprintf(stderr, "injection at %s not reached\n", points[i].name);
            abort();
        }
        if (tunnel_count() || atomic_load(&live_rings) || atomic_load(&live_arrays) ||
            atomic_load(&live_pages) || atomic_load(&live_mappings)) {
            fprintf(stderr, "%s failure left tunnels=%d rings=%d arrays=%d pages=%d maps=%d\n",
                    points[i].name, tunnel_count(), atomic_load(&live_rings), atomic_load(&live_arrays),
                    atomic_load(&live_pages), atomic_load(&live_mappings));
            abort();
        }
        assert(!h.dev[0].tx_ring.ring && !h.dev[0].rx_ring.ring);
        assert(!h.dev[0].tx_ring.frames && !h.dev[0].rx_ring.frames);
        /* Both streams of the NHI open again, without any reload. */
        assert(start(&h.dev[1]) == 0);
        assert(start(&h.dev[0]) == 0);
        assert(tunnel_count() == 2);
        stop(&h.dev[0]);
        stop(&h.dev[1]);
        assert_nothing_held();
    }
    assert(atomic_load(&warnings) == 9); /* every failed open is logged */
}

/* The 32 KiB-128 KiB bookkeeping arrays are not DMA buffers: kvcalloc. */
static void frame_arrays_use_kvcalloc(void)
{
    struct host h;

    host_init(&h, 4096);
    atomic_store(&kvcalloc_calls, 0);
    assert(start(&h.dev[0]) == 0);
    assert(atomic_load(&kvcalloc_calls) == 2 && atomic_load(&live_arrays) == 2);
    stop(&h.dev[0]);
    assert_nothing_held();
}

struct spinner { struct tbstream_dev *s; atomic_bool inside, done; bool result; };
static void *spin_worker(void *data)
{
    struct spinner *w = data;

    atomic_store(&w->inside, true);
    w->result = tbstream_dev_napi_spin(w->s);
    atomic_store(&w->done, true);
    return NULL;
}

static double now_ms(void) { return ktime_get() / 1e6; }

/* 0011 spun up to spin_us with the device mutex held, blocking write() and
 * poll() of the same stream. The spin must run without it. */
static void spin_without_device_mutex(void)
{
    struct host h;
    struct tbstream_dev *s;
    struct spinner w = { 0 };
    pthread_t thread;

    host_init(&h, 64);
    s = &h.dev[0];
    s->cfg_rx_napi = 1;
    s->cfg_spin_us = 1000;
    assert(start(s) == 0 && s->rx_napi && s->rx_spin_us == 1000);
    w.s = s;
    atomic_store(&hold_poll, true);
    atomic_store(&poll_parked, false);
    assert(!pthread_create(&thread, NULL, spin_worker, &w));
    while (!atomic_load(&poll_parked))
        sched_yield();
    /* The reader is inside its spin (parked in tb_ring_poll, rx_lock held).
     * A writer or poll() still gets the device mutex at once. */
    double t0 = now_ms();
    mutex_lock(&s->lock);
    double waited = now_ms() - t0;
    bool spinning = !atomic_load(&w.done);
    tbstream_dev_napi_rx(s); /* poll(): the spinner owns the reap, skip */
    mutex_unlock(&s->lock);
    assert(spinning && waited < 0.5);
    /* A frame arriving during the spin ends it with data available. */
    complete_rx(s->rx_ring.ring, 64);
    atomic_store(&hold_poll, false);
    assert(!pthread_join(thread, NULL));
    assert(w.result && tbstream_ring_available(&s->rx_ring));
    stop(s);
    assert_nothing_held();
}

/* Without data the spin is bounded, re-arms the interrupt and reports none. */
static void spin_is_bounded_and_rearms(void)
{
    struct host h;
    struct tbstream_dev *s;

    host_init(&h, 64);
    s = &h.dev[0];
    s->cfg_rx_napi = 1;
    s->cfg_spin_us = 200;
    assert(start(s) == 0);
    atomic_store(&poll_completes, 0);
    double t0 = now_ms();
    assert(!tbstream_dev_napi_spin(s));
    double spent = now_ms() - t0;
    assert(spent >= 0.2 && spent < 50);
    assert(atomic_load(&poll_completes) == 1);
    /* No spin configured, or interrupt mode: nothing to do. */
    s->rx_spin_us = 0;
    assert(!tbstream_dev_napi_spin(s));
    s->rx_napi = false;
    assert(!tbstream_dev_napi_spin(s));
    assert(atomic_load(&poll_completes) == 1);
    s->rx_napi = true;
    stop(s);
    /* Stopped underneath (suspend): no ring, no dereference. */
    s->rx_spin_us = 100;
    assert(!tbstream_dev_napi_spin(s));
    assert_nothing_held();
}

/* irq_cpu pins the stream's vectors; the previous affinity comes back when
 * the stream stops (or its open fails), because the next ring reuses them. */
static void irq_affinity_is_restored(void)
{
    struct host h;
    struct tbstream_dev *s;
    int tx_irq, rx_irq;

    for (int i = 0; i < NR_IRQS; ++i)
        irq_masks[i].bits = 0xffff;
    host_init(&h, 64);
    s = &h.dev[0];
    s->irq_cpu = 3;
    assert(start(s) == 0);
    tx_irq = s->tx_ring.ring->irq; rx_irq = s->rx_ring.ring->irq;
    assert(irq_masks[tx_irq].bits == 1UL << 3 && irq_masks[rx_irq].bits == 1UL << 3);
    stop(s);
    assert(irq_masks[tx_irq].bits == 0xffff && irq_masks[rx_irq].bits == 0xffff);
    assert(!s->irq_pinned[0] && !s->irq_pinned[1]);
    /* A failed open after the rings started restores too. */
    kvcalloc_fail_after = 1;
    assert(start(s) == -ENOMEM);
    assert(irq_masks[tx_irq].bits == 0xffff && irq_masks[rx_irq].bits == 0xffff);
    /* Offline CPU or a refused change: nothing pinned, nothing restored. */
    int before = atomic_load(&infos);
    online_cpus &= ~(1UL << 3);
    assert(start(s) == 0 && !s->irq_pinned[0] && !s->irq_pinned[1]);
    assert(atomic_load(&infos) == before + 1);
    stop(s);
    online_cpus = 0xffff;
    set_affinity_fail = -EINVAL;
    assert(start(s) == 0 && !s->irq_pinned[0] && !s->irq_pinned[1]);
    assert(atomic_load(&infos) == before + 3);
    stop(s);
    set_affinity_fail = 0;
    assert(irq_masks[tx_irq].bits == 0xffff && irq_masks[rx_irq].bits == 0xffff);
    assert_nothing_held();
}

#include "stream_detach_cases.h"
#include "stream_open_cases.h"

int main(int argc, char **argv)
{
    struct { const char *name; void (*run)(void); } cases[] = {
        { "open_failure_is_not_sticky", open_failure_is_not_sticky },
        { "frame_arrays_use_kvcalloc", frame_arrays_use_kvcalloc },
        { "spin_without_device_mutex", spin_without_device_mutex },
        { "spin_is_bounded_and_rearms", spin_is_bounded_and_rearms },
        { "irq_affinity_is_restored", irq_affinity_is_restored },
        { "detach_stops_open_ram_rings", detach_stops_open_ram_rings },
        { "reconnect_refuses_stale_endpoint", reconnect_refuses_stale_endpoint },
        { "blocked_reader_fails_cleanly", blocked_reader_fails_cleanly },
        { "rmdir_with_open_file", rmdir_with_open_file },
        { "detach_without_open_files", detach_without_open_files },
        { "stop_is_idempotent", stop_is_idempotent },
        { "shutdown_detaches", shutdown_detaches },
        { "open_fails_fast_without_domain", open_fails_fast_without_domain },
        { "open_fails_fast_without_xdomain", open_fails_fast_without_xdomain },
        { "open_nonblock_eagain", open_nonblock_eagain },
        { "open_wait_is_bounded", open_wait_is_bounded },
        { "open_waits_for_peer", open_waits_for_peer },
        { "open_wait_ends_when_xdomain_goes", open_wait_ends_when_xdomain_goes },
        { "open_wait_is_interruptible", open_wait_is_interruptible },
        { "open_timeout_settings", open_timeout_settings },
    };

    assert(argc == 2);
    for (unsigned int i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        if (!strcmp(cases[i].name, argv[1])) {
            cases[i].run();
            printf("PASS %s\n", argv[1]);
            return 0;
        }
    }
    return 2;
}
