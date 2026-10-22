/* SPDX-License-Identifier: GPL-2.0-only */
/* Included after the actual tb_remove_xdomains() (tb.c) and
 * tb_domain_remove() with its XDomain unregistration (domain.c). */

struct topology {
    struct tb_nhi nhi;
    struct tb_ctl ctl;
    struct tb tb;
    struct tb_switch root, child;
    struct tb_xdomain a, b, c;
};

/* root: port 0 -> XDomain a, port 1 -> child router, whose port 1 ->
 * XDomain b and port 2 -> XDomain c (handshake never finished: not added). */
static void topology_init(struct topology *t, const struct tb_cm_ops *ops)
{
    memset(t, 0, sizeof(*t));
    memset(&events, 0, sizeof(events));
    nevents = 0;
    t->tb.nhi = &t->nhi;
    t->tb.ctl = &t->ctl;
    t->ctl.running = true;
    t->tb.cm_ops = ops;
    t->tb.dev.registered = true;
    t->tb.root_switch = &t->root;
    t->tb.cm.hotplug_active = true;
    static struct workqueue_struct wq;
    memset(&wq, 0, sizeof(wq));
    t->tb.wq = &wq;
    t->root.registered = t->child.registered = true;
    t->root.ports = 2;
    t->child.ports = 3;
    t->root.port[1].remote = &t->child.port[0];
    t->child.port[0].remote = &t->root.port[1];
    t->child.port[0].upstream = true;
    t->root.port[1].remote->sw = &t->child;
    t->root.port[0].xdomain = &t->a;
    t->child.port[1].xdomain = &t->b;
    t->child.port[2].xdomain = &t->c;
    t->a = (struct tb_xdomain){ .id = 1, .tb = &t->tb, .parent = &t->root, .registered = true,
                                .tunnels = 1, .rings_running = true };
    t->b = (struct tb_xdomain){ .id = 2, .tb = &t->tb, .parent = &t->child, .registered = true,
                                .tunnels = 1, .rings_running = true };
    t->c = (struct tb_xdomain){ .id = 3, .tb = &t->tb, .parent = &t->child };
    all_xdomains[0] = &t->a; all_xdomains[1] = &t->b; all_xdomains[2] = &t->c;
    nxdomains = 3;
}

static void assert_before(const char *first, const char *second)
{
    int a = event_index(first), b = event_index(second);

    if (a < 0 || b < 0 || a >= b) {
        fprintf(stderr, "expected '%s' (%d) before '%s' (%d)\n", first, a, second, b);
        for (int i = 0; i < nevents; ++i)
            fprintf(stderr, "  %s\n", events[i]);
        abort();
    }
}

static const struct tb_cm_ops software_cm = {
    .remove_xdomains = tb_remove_xdomains, .stop = cm_stop, .deinit = cm_deinit,
};

/* NHI unbind, reboot or module unload with XDomains connected. Before, the
 * XDomains were only marked unplugged by the router removal and stayed
 * registered: their services outlived the domain and the module (node A
 * vmcore, 2026-10-19: device_shutdown() called through a stale bus of
 * service 0-2.0). Their service drivers never ran, so RAM rings also stayed
 * up across the DMA path teardown and nhi_shutdown(). */
static void xdomains_unregistered_before_stop(void)
{
    struct topology t;

    topology_init(&t, &software_cm);
    tb_domain_remove(&t.tb);
    assert(t.nhi.dma_shutdown && !t.tb.cm.hotplug_active && !t.tb.lock.held);
    /* Unregistered once, without tb->lock, on the domain workqueue, while
     * the parent routers and the DMA tunnels still existed. */
    assert(!strcmp(events[event_index("unregister 1")], "unregister 1 parent=1 tunnels=1 lock=0 wq=1"));
    assert(!strcmp(events[event_index("unregister 2")], "unregister 2 parent=1 tunnels=1 lock=0 wq=1"));
    assert(event_index("unregister 3") < 0 && t.c.freed);
    assert(t.a.freed && t.b.freed && t.a.removed && t.b.removed && t.c.removed);
    assert(t.a.is_unplugged && t.b.is_unplugged && t.c.is_unplugged);
    /* The service drivers stopped their rings before the paths went down. */
    assert(!strcmp(events[event_index("deactivate tunnels of 1")], "deactivate tunnels of 1 rings_running=0"));
    assert(!strcmp(events[event_index("deactivate tunnels of 2")], "deactivate tunnels of 2 rings_running=0"));
    assert_before("xdomain_remove 1", "unregister 1");
    assert_before("xdomain_remove 2", "unregister 1");
    assert_before("unregister 2", "deactivate tunnels of 1");
    assert_before("stop", "ctl_stop");
    assert_before("ctl_stop", "flush_workqueue");
    assert_before("flush_workqueue", "cancel_teardown_reset");
    assert_before("cancel_teardown_reset", "retire");
    assert_before("retire", "deinit");
    assert_before("deinit", "domain_unregister");
    assert(!t.root.registered && !t.child.registered && !t.tb.dev.registered);
}

/* A hotplug work queued before removal runs first on the ordered queue and
 * unregisters what it finds; the removal's own pass then finds nothing left
 * and nothing is unregistered twice. */
static struct topology *racing;
static void hotplug_work(struct work_struct *w)
{
    (void)w;
    log_event("hotplug_work");
    tb_domain_unregister_unplugged_xdomains(&racing->tb);
}

static void racing_hotplug_work(void)
{
    struct topology t;
    struct work_struct w;

    topology_init(&t, &software_cm);
    racing = &t;
    INIT_WORK_ONSTACK(&w, hotplug_work);
    queue_work(t.tb.wq, &w);
    tb_domain_remove(&t.tb);
    assert_before("hotplug_work", "unregister 1");
    assert(!strcmp(events[event_index("unregister 1")], "unregister 1 parent=1 tunnels=1 lock=0 wq=1"));
    assert(t.a.freed && t.b.freed);
}

/* A CM without the hook (firmware CM) keeps the previous order. */
static void cm_without_hook(void)
{
    static const struct tb_cm_ops firmware_cm = { .stop = cm_stop, .deinit = cm_deinit };
    struct topology t;

    topology_init(&t, &firmware_cm);
    tb_domain_remove(&t.tb);
    assert(event_index("unregister") < 0);
    assert(!strcmp(events[event_index("deactivate tunnels of 1")], "deactivate tunnels of 1 rings_running=1"));
    assert(t.a.removed && t.a.registered); /* left to that CM, as before */
}

int main(int argc, char **argv)
{
    struct { const char *name; void (*run)(void); } cases[] = {
        { "xdomains_unregistered_before_stop", xdomains_unregistered_before_stop },
        { "racing_hotplug_work", racing_hotplug_work },
        { "cm_without_hook", cm_without_hook },
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
