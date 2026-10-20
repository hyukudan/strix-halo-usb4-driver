/* SPDX-License-Identifier: GPL-2.0-only */
/* Included by stream_cases.h. The stream (its XDomain service) goes away
 * while a file with RAM rings is open: peer unplug or reboot, domain or NHI
 * removal, configfs rmdir, system shutdown. Runs the actual detach, rmdir,
 * shutdown and file operations of stream.c. Before the fix the rings stayed
 * alive until the file was closed, after the NHI could be gone; the close
 * then stopped them (cluster2, 2026-10-19: "interrupt for TX ring N is
 * already disabled"), and a reconnect let the stale file reach the new
 * stream with no rings at all. */

struct endpoint {
    struct tb_nhi nhi;
    struct tb tb;
    struct tb_xdomain xd[2];    /* the peer, before and after a reconnect */
    struct tb_service svc[2];
    struct tbstream stream[2];
    struct tbstream_group sg;
    struct tbstream_dev dev;
    struct file file;
};

static void endpoint_init(struct endpoint *e, int mode)
{
    struct tbstream_dev *s = &e->dev;

    memset(e, 0, sizeof(*e));
    memset(tunnels, 0, sizeof(tunnels));
    e->tb.nhi = &e->nhi;
    for (int i = 0; i < 2; ++i) {
        e->xd[i].tb = &e->tb;
        e->svc[i].xd = &e->xd[i];
        e->svc[i].drvdata = &e->stream[i];
        e->stream[i].svc = &e->svc[i];
    }
    mutex_init(&e->sg.lock);
    INIT_LIST_HEAD(&e->sg.dev_list);
    s->group.cg_item.ci_name = "s0";
    s->domain_index = 1; s->route = 2;
    s->cfg_open_timeout_ms = -1;
    s->cfg_latency_qos_us = -1;
    s->misc.name = "tbstream0";
    s->in_hopid = 8; s->out_hopid = 9;
    s->ring_size = 64; s->throttling = 8192;
    s->cfg_rx_napi = mode == 1; s->cfg_spin_us = mode == 1 ? 1000 : 0;
    s->busy_poll = mode == 2;
    s->irq_cpu = -1;
    mutex_init(&s->lock); mutex_init(&s->rx_lock);
    list_add_tail(&s->list, &e->sg.dev_list);
    found_group = &e->sg;
    e->file.private_data = &s->misc;
    /* The service appears: probe attaches it to the existing endpoint. */
    tbstream_group_attach_stream(&e->stream[0]);
    assert(s->stream == &e->stream[0] && e->xd[0].hopids == 2);
}

/* The cable is pulled: the CM tears the DMA tunnels down, then the service
 * is removed (tbstream_remove -> tbstream_group_detach_stream). */
static void unplug_and_remove(struct endpoint *e, int peer)
{
    e->xd[peer].is_unplugged = true;
    for (int i = 0; i < 8; ++i)
        if (tunnels[i].xd == &e->xd[peer])
            tunnels[i].used = false;
    tbstream_group_detach_stream(&e->stream[peer]);
}

static ssize_t file_read(struct endpoint *e, unsigned char *buf, size_t size)
{
    struct kiocb k = { &e->file, 0 };
    struct iov_iter it = { buf, size };

    return tbstream_dev_fops_read_iter(&k, &it);
}

static ssize_t file_write(struct endpoint *e, unsigned char *buf, size_t size)
{
    struct kiocb k = { &e->file, 0 };
    struct iov_iter it = { buf, size };

    return tbstream_dev_fops_write_iter(&k, &it);
}

static void assert_stale_file(struct endpoint *e)
{
    unsigned char buf[64] = { 0 };
    int stops = atomic_load(&ring_stops), frames = atomic_load(&tx_frames);

    assert(file_read(e, buf, sizeof(buf)) == -ENODEV);
    assert(file_write(e, buf, sizeof(buf)) == -ENODEV);
    if (!e->dev.busy_poll)
        assert(tbstream_dev_fops_poll(&e->file, NULL) == (EPOLLHUP | EPOLLERR));
    assert(atomic_load(&ring_stops) == stops && atomic_load(&tx_frames) == frames);
}

static void assert_rings_gone(struct endpoint *e)
{
    assert(!e->dev.tx_ring.ring && !e->dev.rx_ring.ring);
    assert(!e->dev.tx_ring.frames && !e->dev.rx_ring.frames);
    assert(!atomic_load(&live_rings) && !atomic_load(&live_arrays));
    assert(!atomic_load(&live_pages) && !atomic_load(&live_mappings));
    assert(!qos_live);
}

static void close_stale_file(struct endpoint *e)
{
    int stops = atomic_load(&ring_stops), frames = atomic_load(&tx_frames);

    assert(tbstream_dev_fops_release(NULL, &e->file) == 0);
    /* No second stop, no CLOSE through a ring that no longer exists. */
    assert(atomic_load(&ring_stops) == stops && atomic_load(&tx_frames) == frames);
    assert(!e->dev.users && !e->dev.detached);
}

/* Interrupts, rx_napi with spin, busy_poll: rings stop at service removal,
 * with data in flight both ways, and the file then fails cleanly. */
static void detach_stops_open_ram_rings(void)
{
    for (int mode = 0; mode < 3; ++mode) {
        struct endpoint e;
        unsigned char buf[64] = { 1 };
        int frames;

        endpoint_init(&e, mode);
        assert(tbstream_dev_fops_open(NULL, &e.file) == 0);
        assert(e.dev.users == 1 && e.dev.tx_ring.ring && tunnel_count() == 1);
        assert(file_write(&e, buf, sizeof(buf)) == sizeof(buf));
        complete_rx(e.dev.rx_ring.ring, 64);
        frames = atomic_load(&tx_frames);
        unplug_and_remove(&e, 0);
        assert(e.dev.detached && !e.dev.stream && !e.sg.stream);
        assert_rings_gone(&e);
        assert(atomic_load(&tx_frames) == frames); /* no CLOSE to a gone peer */
        assert(!e.xd[0].hopids && !tunnel_count());
        assert_stale_file(&e);
        close_stale_file(&e);
        assert(!atomic_load(&stream_refs) && !atomic_load(&dev_refs));
    }
}

/* The peer comes back while the stale file is open. The endpoint attaches
 * to the new stream, but the stale file must not reach it, and a new open
 * must not share the stale file's count (it would skip starting rings). */
static void reconnect_refuses_stale_endpoint(void)
{
    struct endpoint e;
    struct file second;
    bool on_new_peer = false;

    endpoint_init(&e, 0);
    assert(tbstream_dev_fops_open(NULL, &e.file) == 0);
    unplug_and_remove(&e, 0);
    tbstream_group_attach_stream(&e.stream[1]);
    assert(e.dev.stream == &e.stream[1] && e.dev.detached);
    mutex_lock(&e.dev.lock);
    assert(!tbstream_dev_valid(&e.dev)); /* the endpoint itself is valid again */
    mutex_unlock(&e.dev.lock);
    assert_stale_file(&e);
    assert(!e.dev.tx_ring.ring && !tunnel_count());
    second = (struct file){ O_NONBLOCK, &e.dev.misc };
    assert(tbstream_dev_fops_open(NULL, &second) == -EBUSY && e.dev.users == 1);
    close_stale_file(&e);
    /* Once the stale file is closed, the endpoint opens on the new peer. */
    assert(tbstream_dev_fops_open(NULL, &second) == 0 && e.dev.tx_ring.ring);
    for (int i = 0; i < 8; ++i)
        on_new_peer |= tunnels[i].used && tunnels[i].xd == &e.xd[1];
    assert(on_new_peer && tunnel_count() == 1);
    assert(tbstream_dev_fops_release(NULL, &second) == 0);
    assert_nothing_held();
    unplug_and_remove(&e, 1);
    assert(!atomic_load(&stream_refs) && !atomic_load(&dev_refs));
}

struct reader { struct endpoint *e; atomic_bool started; ssize_t ret; };
static void *blocking_reader(void *data)
{
    struct reader *r = data;
    unsigned char buf[64];

    atomic_store(&r->started, true);
    r->ret = file_read(r->e, buf, sizeof(buf));
    return NULL;
}

/* A reader sleeping (or spinning with rx_napi) in read() when the stream
 * goes away fails without touching the rings; the detach waits for a
 * spinner instead of freeing the ring under it. On unplug the CM marks the
 * XDomain first, so the reader may already fail with -ENXIO before the
 * service is removed. On shutdown the stream stays valid until the detach,
 * which the reader must then report as -ENODEV. */
static void blocked_reader_fails_cleanly(void)
{
    for (int run = 0; run < 4; ++run) {
        int mode = run & 1;
        bool unplug = run < 2;
        struct endpoint e;
        struct reader r = { .e = &e };
        pthread_t thread;

        endpoint_init(&e, mode);
        assert(tbstream_dev_fops_open(NULL, &e.file) == 0);
        assert(!pthread_create(&thread, NULL, blocking_reader, &r));
        while (!atomic_load(&r.started))
            sched_yield();
        usleep(20000);
        if (unplug)
            unplug_and_remove(&e, 0);
        else
            tbstream_shutdown(&e.svc[0]);
        assert(!pthread_join(thread, NULL));
        if (r.ret != -ENODEV && !(unplug && r.ret == -ENXIO)) {
            fprintf(stderr, "mode %d unplug %d: read returned %zd\n", mode, unplug, r.ret);
            abort();
        }
        assert_rings_gone(&e);
        assert_stale_file(&e);
        close_stale_file(&e);
        if (!unplug)
            unplug_and_remove(&e, 0);
        assert(!atomic_load(&stream_refs) && !atomic_load(&dev_refs));
    }
}

/* rmdir takes the item off the group list, so service removal could no
 * longer find it: its open rings stop at rmdir. The peer is still there and
 * gets a CLOSE, the paths are disabled through the CM and the published
 * properties are withdrawn. */
static void rmdir_with_open_file(void)
{
    struct endpoint e;
    int frames;

    endpoint_init(&e, 0);
    assert(tbstream_dev_fops_open(NULL, &e.file) == 0);
    frames = atomic_load(&tx_frames);
    atomic_store(&item_puts, 0);
    tbstream_dev_drop_item(&e.sg.group, &e.dev.group.cg_item);
    assert(e.dev.removed && e.dev.detached && !e.dev.stream);
    assert(list_empty(&e.sg.dev_list) && atomic_load(&item_puts) == 1);
    assert(atomic_load(&tx_frames) == frames + 1);
    assert(!tunnel_count() && !e.xd[0].hopids);
    assert(published_in == 0 && published_out == 0);
    assert_rings_gone(&e);
    assert_stale_file(&e);
    close_stale_file(&e);
    /* Service removal afterwards neither finds nor touches the item. */
    unplug_and_remove(&e, 0);
    assert(!atomic_load(&stream_refs) && !atomic_load(&dev_refs));
}

/* Nothing open: detach only releases the HopIDs, as before. */
static void detach_without_open_files(void)
{
    struct endpoint e;
    int stops;

    endpoint_init(&e, 0);
    stops = atomic_load(&ring_stops);
    unplug_and_remove(&e, 0);
    assert(atomic_load(&ring_stops) == stops && !e.dev.detached && !e.xd[0].hopids);
    e.file.f_flags = O_NONBLOCK;
    assert(tbstream_dev_fops_open(NULL, &e.file) == -ENXIO);
    assert(!atomic_load(&stream_refs) && !atomic_load(&dev_refs));
}

/* Suspend stopped the rings and resume could not restart them: neither a
 * later detach nor the close may stop them a second time. */
static void stop_is_idempotent(void)
{
    for (int detach = 0; detach < 2; ++detach) {
        struct endpoint e;
        int stops;

        endpoint_init(&e, 0);
        assert(tbstream_dev_fops_open(NULL, &e.file) == 0);
        mutex_lock(&e.dev.lock);
        tbstream_dev_stop(&e.dev);
        mutex_unlock(&e.dev.lock);
        stops = atomic_load(&ring_stops);
        if (detach)
            unplug_and_remove(&e, 0);
        assert(tbstream_dev_fops_release(NULL, &e.file) == 0);
        assert(atomic_load(&ring_stops) == stops && !e.dev.users);
        assert_rings_gone(&e);
        if (!detach)
            unplug_and_remove(&e, 0);
        assert(!atomic_load(&stream_refs) && !atomic_load(&dev_refs));
    }
}

/* Reboot does not remove the service: .shutdown stops the rings first. */
static void shutdown_detaches(void)
{
    struct endpoint e;

    endpoint_init(&e, 0);
    assert(tbstream_dev_fops_open(NULL, &e.file) == 0);
    tbstream_shutdown(&e.svc[0]);
    assert(e.dev.detached && !e.sg.stream && !tunnel_count());
    assert_rings_gone(&e);
    assert_stale_file(&e);
    close_stale_file(&e);
    assert(!atomic_load(&stream_refs) && !atomic_load(&dev_refs));
}
