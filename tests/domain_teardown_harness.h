/* SPDX-License-Identifier: GPL-2.0-only */
/* Doubles for the actual domain removal sequence: tb_domain_remove() and
 * its XDomain unregistration (domain.c) with the CM's tb_remove_xdomains()
 * (tb.c). Routers, XDomains, services, tunnels, the control channel and the
 * ordered domain workqueue are models that log every step, so the cases can
 * check what was still alive when each step ran. */
#ifndef DOMAIN_TEARDOWN_HARNESS_H
#define DOMAIN_TEARDOWN_HARNESS_H
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define container_of(p, t, m) ((t *)((char *)(p) - offsetof(t, m)))
#define typeof __typeof__

/* Event log: one line per step, checked by the cases. */
static char events[64][96];
static int nevents;
#define log_event(...) do { \
    assert(nevents < 64); \
    snprintf(events[nevents++], sizeof(events[0]), __VA_ARGS__); \
} while (0)
static int event_index(const char *prefix)
{
    for (int i = 0; i < nevents; ++i)
        if (!strncmp(events[i], prefix, strlen(prefix)))
            return i;
    return -1;
}

struct mutex { bool held; };
static void mutex_lock(struct mutex *m) { assert(!m->held); m->held = true; }
static void mutex_unlock(struct mutex *m) { assert(m->held); m->held = false; }
typedef struct { int unused; } spinlock_t;
static void spin_lock_irq(spinlock_t *l) { (void)l; }
static void spin_unlock_irq(spinlock_t *l) { (void)l; }

/* An ordered workqueue: queued items run in order when flushed. */
struct work_struct;
typedef void (*work_func_t)(struct work_struct *);
struct workqueue_struct;
struct work_struct { work_func_t func; bool queued; struct workqueue_struct *wq; };
struct workqueue_struct { struct work_struct *items[8]; int n; bool running; };
static struct workqueue_struct *current_wq;
#define INIT_WORK_ONSTACK(w, f) do { (w)->func = (f); (w)->queued = false; } while (0)
static void destroy_work_on_stack(struct work_struct *w) { assert(!w->queued); }
static bool queue_work(struct workqueue_struct *wq, struct work_struct *w)
{
    assert(wq->n < 8 && !w->queued);
    w->queued = true;
    w->wq = wq;
    wq->items[wq->n++] = w;
    return true;
}
static void run_queue(struct workqueue_struct *wq)
{
    for (int i = 0; i < wq->n; ++i) {
        struct work_struct *w = wq->items[i];

        current_wq = wq;
        w->queued = false;
        w->func(w);
        current_wq = NULL;
    }
    wq->n = 0;
}
static bool flush_work(struct work_struct *w)
{
    if (w->queued)
        run_queue(w->wq);
    return true;
}
static void flush_workqueue(struct workqueue_struct *wq) { run_queue(wq); log_event("flush_workqueue"); }

/* Topology. */
struct tb;
struct tb_switch;
struct tb_xdomain {
    int id;
    struct tb *tb;
    struct tb_switch *parent;
    bool registered, is_unplugged, removed, freed;
    int tunnels;        /* DMA tunnels of this XDomain in the CM */
    bool rings_running; /* its service driver's rings */
};
struct tb_port {
    struct tb_switch *sw;
    struct tb_port *remote;
    struct tb_xdomain *xdomain;
    bool upstream;
};
struct tb_switch { int ports; struct tb_port port[3]; bool registered; };
#define tb_switch_for_each_port(sw, p) \
    for ((p) = &(sw)->port[0]; (p) < &(sw)->port[(sw)->ports]; (p)++)
static bool tb_port_has_remote(const struct tb_port *p) { return !p->upstream && p->remote; }

static void tb_xdomain_remove(struct tb_xdomain *xd)
{
    assert(!xd->removed);
    xd->removed = true;
    log_event("xdomain_remove %d", xd->id);
    if (!xd->registered)
        xd->freed = true; /* put_device() of a never added XDomain */
}

struct tb_nhi { spinlock_t lock; bool dma_shutdown; };
struct tb_ctl { bool running; };
struct device { bool registered; };
struct tb_cm { bool hotplug_active; };
struct tb_cm_ops {
    void (*remove_xdomains)(struct tb *tb);
    void (*stop)(struct tb *tb);
    void (*deinit)(struct tb *tb);
};
struct tb {
    struct mutex lock;
    struct tb_nhi *nhi;
    const struct tb_cm_ops *cm_ops;
    struct tb_ctl *ctl;
    struct workqueue_struct *wq;
    struct device dev;
    struct tb_switch *root_switch;
    struct tb_cm cm;
};
static struct tb_cm *tb_priv(struct tb *tb) { return &tb->cm; }

/* The XDomains of the test topology, as the bus would enumerate them. */
static struct tb_xdomain *all_xdomains[4];
static int nxdomains;

/* Service removal: the service driver stops its rings (tbstream_remove). */
static void unregister_xdomain(struct tb_xdomain *xd)
{
    assert(xd->registered && !xd->freed);
    log_event("unregister %d parent=%d tunnels=%d lock=%d wq=%d", xd->id,
              xd->parent->registered, xd->tunnels, xd->tb->lock.held,
              current_wq == xd->tb->wq);
    xd->rings_running = false;
    xd->registered = false;
    xd->freed = true;
}

static int tb_domain_unregister_unplugged_xdomains(struct tb *tb)
{
    int n = 0;

    assert(!tb->lock.held); /* lockdep_assert_not_held() in the real one */
    for (int i = 0; i < nxdomains; ++i) {
        struct tb_xdomain *xd = all_xdomains[i];

        if (xd->tb == tb && xd->registered && xd->is_unplugged) {
            unregister_xdomain(xd);
            ++n;
        }
    }
    return n;
}

/* CM stop: deactivates the DMA tunnels (logging whether rings still ran),
 * then removes the routers the way tb_switch_remove() does. */
static void switch_remove(struct tb_switch *sw)
{
    struct tb_port *port;

    tb_switch_for_each_port(sw, port) {
        if (tb_port_has_remote(port)) {
            switch_remove(port->remote->sw);
        } else if (port->xdomain) {
            port->xdomain->is_unplugged = true;
            tb_xdomain_remove(port->xdomain);
            port->xdomain = NULL;
        }
    }
    sw->registered = false;
}

static void cm_stop(struct tb *tb)
{
    assert(tb->lock.held);
    for (int i = 0; i < nxdomains; ++i) {
        struct tb_xdomain *xd = all_xdomains[i];

        if (xd->tunnels)
            log_event("deactivate tunnels of %d rings_running=%d", xd->id, xd->rings_running);
        xd->tunnels = 0;
    }
    switch_remove(tb->root_switch);
    tb->root_switch = NULL;
    tb->cm.hotplug_active = false;
    log_event("stop");
}

static void tb_ctl_stop(struct tb_ctl *ctl) { ctl->running = false; log_event("ctl_stop"); }
static void tb_dma_retire_domain_clients(struct tb *tb) { (void)tb; log_event("retire"); }
static void cm_deinit(struct tb *tb) { (void)tb; log_event("deinit"); }
static void device_unregister(struct device *dev) { dev->registered = false; log_event("domain_unregister"); }
#endif
