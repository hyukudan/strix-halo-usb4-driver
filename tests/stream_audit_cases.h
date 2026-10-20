/* SPDX-License-Identifier: GPL-2.0-only */
/* Included by stream_cases.h after stream_open_cases.h. Cases for the
 * external audit of 2026-10-19 (A2-A11), run on the actual stream.c. */

/* A2: an open file keeps the device alive, not its stream; detach clears
 * the pointer and drops the stream under the device mutex. Every path that
 * looks inside the stream (tbstream_dev_valid -> tb_service_parent) must
 * therefore hold that mutex: the unlocked pre-checks and the wait conditions
 * of read, write and open may only test whether a stream is attached. */
struct waiter { struct endpoint *e; int what; atomic_bool started; ssize_t ret; };
static void *waiter_thread(void *data)
{
    struct waiter *w = data;
    unsigned char buf[64] = { 0 };

    atomic_store(&w->started, true);
    if (w->what == 0)
        w->ret = file_read(w->e, buf, sizeof(buf));
    else
        w->ret = file_write(w->e, buf, sizeof(buf));
    return NULL;
}

static void stream_deref_under_lock(void)
{
    for (int what = 0; what < 2; ++what) {
        struct endpoint e;
        struct waiter w = { .e = &e, .what = what };
        unsigned char buf[64] = { 7 };
        pthread_t thread;

        /* rx_napi: the reader reaps the completion itself */
        endpoint_init(&e, 1);
        stream_deref_lock = &e.dev.lock;
        assert(tbstream_dev_fops_open(NULL, &e.file) == 0);
        assert(file_write(&e, buf, sizeof(buf)) == sizeof(buf));
        complete_rx(e.dev.rx_ring.ring, 64);
        assert(file_read(&e, buf, sizeof(buf)) == 64);
        assert(tbstream_dev_fops_poll(&e.file, NULL) & EPOLLOUT);
        if (what == 1) {
            /* fill TX so the writer sleeps */
            while (tbstream_ring_available(&e.dev.tx_ring))
                assert(file_write(&e, buf, sizeof(buf)) == sizeof(buf));
        }
        assert(!pthread_create(&thread, NULL, waiter_thread, &w));
        while (!atomic_load(&w.started))
            sched_yield();
        usleep(20000);
        /* The service goes away under the sleeper (its wait condition runs
         * concurrently with the detach). */
        unplug_and_remove(&e, 0);
        assert(!pthread_join(thread, NULL));
        assert(w.ret == -ENXIO || w.ret == -ENODEV);
        close_stale_file(&e);
        /* Open waiting for a peer: the wait condition must not look inside
         * the stream either. */
        atomic_store(&presence_result, 0);
        e.dev.cfg_open_timeout_ms = 100;
        assert(tbstream_dev_fops_open(NULL, &e.file) == -ETIMEDOUT);
        atomic_store(&presence_result, -ENXIO);
        stream_deref_lock = NULL;
        assert(!atomic_load(&stream_refs) && !atomic_load(&dev_refs));
    }
}

/* A second device in the endpoint's group, opened on the same stream. */
static void second_dev(struct endpoint *e, struct tbstream_dev *s, struct file *f)
{
    memset(s, 0, sizeof(*s));
    s->group.cg_item.ci_name = "s1";
    s->misc.name = "tbstream1";
    s->domain_index = 1; s->route = 2;
    s->in_hopid = 10; s->out_hopid = 11;
    s->ring_size = 64; s->throttling = 8192;
    s->cfg_rx_napi = 0; s->cfg_spin_us = 0; s->cfg_open_timeout_ms = -1;
    s->cfg_latency_qos_us = -1;
    s->irq_cpu = -1;
    mutex_init(&s->lock); mutex_init(&s->rx_lock);
    list_add_tail(&s->list, &e->sg.dev_list);
    tbstream_dev_attach_stream(s, &e->sg);
    *f = (struct file){ 0, &s->misc };
}

/* A3: resume could not restart the rings of an open file. Before, the
 * error returned at once: the file kept users != 0 with NULL rings (busy_poll
 * read reached tb_ring_poll(NULL), other modes waited forever, a new open
 * shared the count and skipped start) and the devices after it in the group
 * were never resumed. Now the file fails like a detached one and the others
 * resume. */
static void resume_failure_fails_open_files(void)
{
    struct endpoint e;
    struct tbstream_dev other;
    struct file other_file, again;
    unsigned char buf[64] = { 3 };
    int stops;

    endpoint_init(&e, 2);   /* busy_poll on the first device */
    second_dev(&e, &other, &other_file);
    assert(tbstream_dev_fops_open(NULL, &e.file) == 0);
    assert(tbstream_dev_fops_open(NULL, &other_file) == 0);
    assert(tbstream_suspend(&e.svc[0].dev) == 0);
    assert(!e.dev.tx_ring.ring && !other.tx_ring.ring);
    ring_fail_after = 0;    /* the first device's TX ring allocation fails */
    assert(tbstream_resume(&e.svc[0].dev) == -ENOMEM);
    assert(ring_fail_after == -1);
    /* The failed device: a terminal state, users woken. */
    assert(e.dev.detached && !e.dev.tx_ring.ring);
    assert_stale_file(&e);
    again = (struct file){ O_NONBLOCK, &e.dev.misc };
    assert(tbstream_dev_fops_open(NULL, &again) == -EBUSY && e.dev.users == 1);
    /* The device after it was resumed anyway. */
    assert(other.tx_ring.ring && other.rx_ring.ring && tunnel_count() == 1);
    {
        struct kiocb k = { &other_file, 0 };
        struct iov_iter it = { buf, sizeof(buf) };

        assert(tbstream_dev_fops_write_iter(&k, &it) == sizeof(buf));
    }
    /* Closing the failed file stops nothing; then it opens afresh. */
    stops = atomic_load(&ring_stops);
    assert(tbstream_dev_fops_release(NULL, &e.file) == 0);
    assert(atomic_load(&ring_stops) == stops && !e.dev.detached);
    assert(tbstream_dev_fops_open(NULL, &e.file) == 0 && e.dev.tx_ring.ring);
    assert(tbstream_dev_fops_release(NULL, &e.file) == 0);
    assert(tbstream_dev_fops_release(NULL, &other_file) == 0);
    assert_nothing_held();
    unplug_and_remove(&e, 0);
    assert(!atomic_load(&stream_refs) && !atomic_load(&dev_refs));
}

/* A4: the first RX frame completes while open is still posting buffers.
 * Before, the counters were zeroed after the posting loop and the frame's
 * prod increment was lost: its data never reached the reader. */
static void early_rx_completion_is_kept(void)
{
    struct endpoint e;
    unsigned char buf[128];

    endpoint_init(&e, 0);
    rx_early_complete_after = 0;
    assert(tbstream_dev_fops_open(NULL, &e.file) == 0);
    assert(rx_early_complete_after == -1);
    assert(e.dev.rx_ring.prod == 1 && e.dev.rx_ring.cons == 0);
    e.file.f_flags = O_NONBLOCK;
    assert(file_read(&e, buf, sizeof(buf)) == 64);
    assert(file_read(&e, buf, sizeof(buf)) == -EAGAIN);
    assert(tbstream_dev_fops_release(NULL, &e.file) == 0);
    assert_nothing_held();
    unplug_and_remove(&e, 0);
}

/* A6: a frame with a CRC error or a buffer overrun was counted, nobody was
 * woken, and read() returned its contents as valid data: a silent hole or
 * garbage in a byte stream. Now the data before it is delivered, then every
 * read fails with -EIO until the file is closed, and poll reports it. */
static void damaged_rx_frame_fails_stream(void)
{
    unsigned flags[] = { RING_DESC_CRC_ERROR, RING_DESC_BUFFER_OVERRUN };

    for (int i = 0; i < 2; ++i) {
        struct endpoint e;
        unsigned char buf[256];
        int woken;

        endpoint_init(&e, 1);
        assert(tbstream_dev_fops_open(NULL, &e.file) == 0);
        e.file.f_flags = O_NONBLOCK;
        complete_rx(e.dev.rx_ring.ring, 64);
        complete_rx_flags(e.dev.rx_ring.ring, 64, flags[i]);
        complete_rx(e.dev.rx_ring.ring, 64);
        woken = atomic_load(&wakeups);
        /* One good frame, then the hole: only the good bytes. */
        assert(file_read(&e, buf, sizeof(buf)) == 64);
        assert(atomic_load(&wakeups) > woken); /* the damaged frame woke readers */
        assert(file_read(&e, buf, sizeof(buf)) == -EIO);
        assert(file_read(&e, buf, sizeof(buf)) == -EIO);
        assert(tbstream_dev_fops_poll(&e.file, NULL) & EPOLLERR);
        assert(tbstream_dev_fops_release(NULL, &e.file) == 0);
        /* A new open starts a new stream. */
        e.file.f_flags = 0;
        assert(tbstream_dev_fops_open(NULL, &e.file) == 0 && !e.dev.rx_failed);
        e.file.f_flags = O_NONBLOCK;
        complete_rx(e.dev.rx_ring.ring, 64);
        assert(file_read(&e, buf, sizeof(buf)) == 64);
        assert(!(tbstream_dev_fops_poll(&e.file, NULL) & EPOLLERR));
        assert(tbstream_dev_fops_release(NULL, &e.file) == 0);
        assert_nothing_held();
        unplug_and_remove(&e, 0);
    }
    /* Interrupt mode: the ring work runs the damaged frame's callback alone,
     * and that callback must wake a sleeping reader (it used to only log). */
    for (int i = 0; i < 2; ++i) {
        struct endpoint e;
        struct ring_frame *f;
        unsigned char buf[64];
        int woken;

        endpoint_init(&e, 0);
        assert(tbstream_dev_fops_open(NULL, &e.file) == 0);
        complete_rx_flags(e.dev.rx_ring.ring, 64, flags[i]);
        f = tb_ring_poll(e.dev.rx_ring.ring);
        assert(f);
        woken = atomic_load(&wakeups);
        f->callback(e.dev.rx_ring.ring, f, false);
        assert(atomic_load(&wakeups) == woken + 1);
        e.file.f_flags = O_NONBLOCK;
        assert(file_read(&e, buf, sizeof(buf)) == -EIO);
        assert(tbstream_dev_fops_release(NULL, &e.file) == 0);
        assert_nothing_held();
        unplug_and_remove(&e, 0);
    }
}

struct tx_completer { struct tb_ring *ring; int delay_ms; };
static void *tx_completer_thread(void *data)
{
    struct tx_completer *c = data;

    usleep(c->delay_ms * 1000);
    complete_tx(c->ring);
    return NULL;
}

static ring_cb last_posted_is_close(struct tb_ring *r)
{
    struct ring_frame *f;

    pthread_mutex_lock(&r->lock);
    f = list_empty(&r->posted) ? NULL : container_of(r->posted.prev, struct ring_frame, list);
    pthread_mutex_unlock(&r->lock);
    return f && f->eof == TBSTREAM_CLOSE ? f->callback : NULL;
}

/* A7: with the TX ring full in interrupt mode, CLOSE was tried once and
 * silently dropped (the comment claimed two tries): the peer's reader never
 * saw EOF. Now close waits a bounded time for a TX slot, and says so if
 * none frees up. */
static void close_waits_for_tx_slot(void)
{
    struct endpoint e;
    struct tx_completer c;
    unsigned char buf[64] = { 1 };
    pthread_t thread;
    double t0;
    int frames, warned;

    /* A data frame completes 50 ms into the close: CLOSE goes out. */
    endpoint_init(&e, 0);
    assert(tbstream_dev_fops_open(NULL, &e.file) == 0);
    while (tbstream_ring_available(&e.dev.tx_ring))
        assert(file_write(&e, buf, sizeof(buf)) == sizeof(buf));
    frames = atomic_load(&tx_frames);
    c = (struct tx_completer){ e.dev.tx_ring.ring, 50 };
    assert(!pthread_create(&thread, NULL, tx_completer_thread, &c));
    {
        /* Observe the CLOSE before stop() cancels the posted frames. */
        struct tb_ring *ring = e.dev.tx_ring.ring;

        mutex_lock(&e.dev.lock);
        t0 = ktime_get() / 1e6;
        assert(tbstream_dev_send_close(&e.dev) == 0);
        assert(ktime_get() / 1e6 - t0 >= 40);
        assert(atomic_load(&tx_frames) == frames + 1 && last_posted_is_close(ring));
        mutex_unlock(&e.dev.lock);
    }
    assert(!pthread_join(thread, NULL));
    assert(tbstream_dev_fops_release(NULL, &e.file) == 0);
    assert_nothing_held();
    unplug_and_remove(&e, 0);

    /* Nothing completes: close gives up after the bound, logs, and still
     * stops and frees everything. */
    endpoint_init(&e, 0);
    assert(tbstream_dev_fops_open(NULL, &e.file) == 0);
    while (tbstream_ring_available(&e.dev.tx_ring))
        assert(file_write(&e, buf, sizeof(buf)) == sizeof(buf));
    frames = atomic_load(&tx_frames);
    warned = atomic_load(&warnings);
    t0 = ktime_get() / 1e6;
    assert(tbstream_dev_fops_release(NULL, &e.file) == 0);
    assert(ktime_get() / 1e6 - t0 >= 450);
    assert(atomic_load(&tx_frames) == frames && atomic_load(&warnings) == warned + 1);
    assert_nothing_held();
    unplug_and_remove(&e, 0);
}

struct zero_io { struct endpoint *e; int write; atomic_bool done; ssize_t ret; };
static void *zero_io_thread(void *data)
{
    struct zero_io *z = data;
    unsigned char b[1];

    z->ret = z->write ? file_write(z->e, b, 0) : file_read(z->e, b, 0);
    atomic_store(&z->done, true);
    return NULL;
}

/* Runs a blocking zero-length read/write; it must return 0 at once even
 * with nothing to read or no TX slot. Before, it slept for data. */
static void zero_length_returns_at_once(struct endpoint *e, int write)
{
    struct zero_io z = { .e = e, .write = write };
    pthread_t thread;

    assert(!pthread_create(&thread, NULL, zero_io_thread, &z));
    for (int i = 0; i < 200 && !atomic_load(&z.done); ++i)
        usleep(1000);
    if (!atomic_load(&z.done)) {
        fprintf(stderr, "zero-length %s blocked\n", write ? "write" : "read");
        abort();
    }
    assert(!pthread_join(thread, NULL) && z.ret == 0);
}

/* A8: read/write returned an error after already moving bytes (the caller
 * then lost them), a partial copy was not accounted, and a zero-length read
 * or write could block. */
static void partial_io_is_accounted(void)
{
    struct endpoint e;
    static unsigned char buf[2 * TB_MAX_FRAME_SIZE];
    int frames;

    endpoint_init(&e, 1);
    assert(tbstream_dev_fops_open(NULL, &e.file) == 0);
    zero_length_returns_at_once(&e, 0);
    e.file.f_flags = O_NONBLOCK;

    /* Read: the buffer faults 10 bytes into the second frame. The 74 bytes
     * delivered are reported; the rest of that frame comes next time. */
    complete_rx(e.dev.rx_ring.ring, 64);
    complete_rx(e.dev.rx_ring.ring, 64);
    iov_budget = 64 + 10;
    assert(file_read(&e, buf, sizeof(buf)) == 74);
    iov_budget = -1;
    assert(file_read(&e, buf, sizeof(buf)) == 54);
    /* A fault before any byte is an error, and the frame stays. */
    complete_rx(e.dev.rx_ring.ring, 64);
    iov_budget = 0;
    assert(file_read(&e, buf, sizeof(buf)) == -EFAULT);
    iov_budget = -1;
    assert(file_read(&e, buf, sizeof(buf)) == 64);

    /* Write: the source faults inside the second frame; only the first is
     * sent and reported, the iterator gives the rest back. */
    frames = atomic_load(&tx_frames);
    iov_budget = TB_MAX_FRAME_SIZE + 10;
    assert(file_write(&e, buf, 2 * TB_MAX_FRAME_SIZE) == TB_MAX_FRAME_SIZE);
    iov_budget = -1;
    assert(atomic_load(&tx_frames) == frames + 1);
    /* The iterator moved exactly as far as reported. */
    {
        struct kiocb k = { &e.file, 0 };
        struct iov_iter it = { buf, 2 * TB_MAX_FRAME_SIZE };

        iov_budget = TB_MAX_FRAME_SIZE + 10;
        assert(tbstream_dev_fops_write_iter(&k, &it) == TB_MAX_FRAME_SIZE);
        iov_budget = -1;
        assert(it.count == TB_MAX_FRAME_SIZE && it.buf == buf + TB_MAX_FRAME_SIZE);
    }
    /* The ring refuses the second frame: the first is still reported. */
    tx_fail_after = 1;
    assert(file_write(&e, buf, 2 * TB_MAX_FRAME_SIZE) == TB_MAX_FRAME_SIZE);
    assert(tx_fail_after == -1);
    /* ... and a refusal before any byte is the error itself. */
    tx_fail_after = 0;
    assert(file_write(&e, buf, 64) == -ESHUTDOWN);

    /* Zero-length write with the TX ring full. */
    while (tbstream_ring_available(&e.dev.tx_ring))
        assert(file_write(&e, buf, 64) == 64);
    e.file.f_flags = 0;
    zero_length_returns_at_once(&e, 1);
    assert(tbstream_dev_fops_release(NULL, &e.file) == 0);
    assert_nothing_held();
    unplug_and_remove(&e, 0);
}

/* The far end of the cable: completes TX frames as they are posted. */
struct wire { struct tb_ring *ring; atomic_bool stop; };
static void *wire_thread(void *data)
{
    struct wire *w = data;

    while (!atomic_load(&w->stop))
        if (!complete_tx(w->ring))
            usleep(200);
    return NULL;
}

/* A10: close waited the full 500 ms for posted RX buffers that only
 * complete when data arrives. TX is still drained; RX is stopped (the stop
 * cancels the posted buffers and waits for their callbacks). */
static void close_does_not_flush_rx(void)
{
    struct endpoint e;
    unsigned char buf[64] = { 0 };
    struct wire w;
    pthread_t thread;
    double t0, spent;

    endpoint_init(&e, 0);
    for (int written = 0; written < 2; ++written) {
        assert(tbstream_dev_fops_open(NULL, &e.file) == 0);
        if (written)
            assert(file_write(&e, buf, sizeof(buf)) == sizeof(buf));
        w = (struct wire){ .ring = e.dev.tx_ring.ring };
        assert(!pthread_create(&thread, NULL, wire_thread, &w));
        atomic_store(&rx_flushes, 0);
        t0 = ktime_get() / 1e6;
        assert(tbstream_dev_fops_release(NULL, &e.file) == 0);
        spent = ktime_get() / 1e6 - t0;
        atomic_store(&w.stop, true);
        assert(!pthread_join(thread, NULL));
        /* The data and the CLOSE went out; no wait on the idle RX ring. */
        assert(spent < 200 && !atomic_load(&rx_flushes));
        assert_nothing_held();
    }
    unplug_and_remove(&e, 0);
}

/* A11: the module parameter could set any spin, while configfs stopped at
 * TBSTREAM_DEV_MAX_SPIN_US: a reader then busy-polled with rx_lock held for
 * as long as the parameter said. */
static void spin_us_parameter_is_bounded(void)
{
    struct kernel_param kp = { &spin_us };
    struct endpoint e;

    assert(spin_us_set("1000", &kp) == 0 && spin_us == 1000);
    assert(spin_us_set("1001", &kp) == -EINVAL && spin_us == 1000);
    assert(spin_us_set("4294967295", &kp) == -EINVAL && spin_us == 1000);
    assert(spin_us_set("x", &kp) == -EINVAL && spin_us == 1000);
    assert(spin_us_set("25", &kp) == 0 && spin_us == 25);
    /* A value that got there anyway (written before the setter, a racing
     * write) is still capped where a stream uses it. */
    spin_us = 100000;
    endpoint_init(&e, 1);
    e.dev.cfg_spin_us = -1;
    assert(tbstream_dev_fops_open(NULL, &e.file) == 0);
    assert(e.dev.rx_spin_us == TBSTREAM_DEV_MAX_SPIN_US);
    assert(tbstream_dev_fops_release(NULL, &e.file) == 0);
    spin_us = 0;
    assert_nothing_held();
    unplug_and_remove(&e, 0);
}

/* Review of A6: read() took a CLOSE as EOF before checking its descriptor,
 * so a CLOSE with a CRC error or an overrun ended the stream "normally".
 * The damage is checked first: data before it, then EIO, never EOF. */
static void damaged_close_is_not_eof(void)
{
    unsigned flags[] = { RING_DESC_CRC_ERROR, RING_DESC_BUFFER_OVERRUN };

    for (int i = 0; i < 2; ++i) {
        struct endpoint e;
        unsigned char buf[256];

        endpoint_init(&e, 1);
        assert(tbstream_dev_fops_open(NULL, &e.file) == 0);
        e.file.f_flags = O_NONBLOCK;
        complete_rx(e.dev.rx_ring.ring, 64);
        complete_rx_frame(e.dev.rx_ring.ring, 64, TBSTREAM_CLOSE, flags[i]);
        assert(file_read(&e, buf, sizeof(buf)) == 64);
        assert(file_read(&e, buf, sizeof(buf)) == -EIO);
        assert(!e.dev.closed && e.dev.rx_failed);
        assert(file_read(&e, buf, sizeof(buf)) == -EIO);
        assert(tbstream_dev_fops_poll(&e.file, NULL) & EPOLLERR);
        assert(tbstream_dev_fops_release(NULL, &e.file) == 0);
        assert_nothing_held();
        unplug_and_remove(&e, 0);
    }
    /* An intact CLOSE is still the end of the stream. */
    {
        struct endpoint e;
        unsigned char buf[64];

        endpoint_init(&e, 1);
        assert(tbstream_dev_fops_open(NULL, &e.file) == 0);
        e.file.f_flags = O_NONBLOCK;
        complete_rx_frame(e.dev.rx_ring.ring, 64, TBSTREAM_CLOSE, 0);
        assert(file_read(&e, buf, sizeof(buf)) == 0 && e.dev.closed && !e.dev.rx_failed);
        assert(tbstream_dev_fops_release(NULL, &e.file) == 0);
        assert_nothing_held();
        unplug_and_remove(&e, 0);
    }
}

/* Review of A3: the resume test only failed the first allocation. Fail each
 * step of tbstream_dev_start() in turn, up to the RX buffers that are posted
 * after the paths are enabled: the device fails like a detached one with
 * nothing left allocated or tunnelled, the other device resumes, a new open
 * works. */
struct resume_fault { const char *what; int *after; int value; };

static void resume_failure_at_every_step(void)
{
    struct resume_fault faults[] = {
        { "TX ring", &ring_fail_after, 0 },
        { "TX frame array", &kvcalloc_fail_after, 0 },
        { "TX page", &page_fail_after, 0 },
        { "RX ring", &ring_fail_after, 1 },
        { "paths", &enable_fail, -EIO },
        { "RX frame array", &kvcalloc_fail_after, 1 },
    };

    for (unsigned int i = 0; i < sizeof(faults) / sizeof(faults[0]); ++i) {
        struct endpoint e;
        struct tbstream_dev other;
        struct file other_file;
        unsigned char buf[64] = { 7 };
        int rings, pages, arrays, maps;

        endpoint_init(&e, 0);
        second_dev(&e, &other, &other_file);
        /* What the other device alone holds when it runs. */
        assert(tbstream_dev_fops_open(NULL, &other_file) == 0);
        rings = atomic_load(&live_rings); pages = atomic_load(&live_pages);
        arrays = atomic_load(&live_arrays); maps = atomic_load(&live_mappings);
        assert(tbstream_dev_fops_open(NULL, &e.file) == 0);
        assert(tbstream_suspend(&e.svc[0].dev) == 0);
        *faults[i].after = faults[i].value;
        assert(tbstream_resume(&e.svc[0].dev) < 0);
        if (faults[i].after != &enable_fail)
            assert(*faults[i].after == -1); /* the fault was reached */
        assert(!enable_fail);
        assert(e.dev.detached && !e.dev.tx_ring.ring && !e.dev.rx_ring.ring);
        assert(atomic_load(&live_rings) == rings && atomic_load(&live_pages) == pages);
        assert(atomic_load(&live_arrays) == arrays && atomic_load(&live_mappings) == maps);
        assert(tunnel_count() == 1);
        assert_stale_file(&e);
        {
            struct kiocb k = { &other_file, 0 };
            struct iov_iter it = { buf, sizeof(buf) };

            assert(tbstream_dev_fops_write_iter(&k, &it) == sizeof(buf));
        }
        assert(tbstream_dev_fops_release(NULL, &e.file) == 0);
        assert(tbstream_dev_fops_open(NULL, &e.file) == 0 && e.dev.tx_ring.ring && tunnel_count() == 2);
        assert(tbstream_dev_fops_release(NULL, &e.file) == 0);
        assert(tbstream_dev_fops_release(NULL, &other_file) == 0);
        assert_nothing_held();
        unplug_and_remove(&e, 0);
        assert(!atomic_load(&stream_refs) && !atomic_load(&dev_refs));
    }
}

/* 0042: an open host stream holds one CPU latency request while its rings
 * run: from a successful start to the stop of the last close, a suspend or
 * a detach. A failed open or resume holds none. */
static void latency_qos_follows_rings(void)
{
    struct endpoint e;
    struct file second;

    qos_live = qos_adds = 0;
    endpoint_init(&e, 0);
    assert(tbstream_dev_fops_open(NULL, &e.file) == 0);
    assert(!qos_live && !qos_adds);                 /* default: none */
    assert(tbstream_dev_fops_release(NULL, &e.file) == 0);

    e.dev.cfg_latency_qos_us = 20;
    assert(tbstream_dev_fops_open(NULL, &e.file) == 0);
    assert(qos_live == 1 && e.dev.qos.active && e.dev.qos.value == 20);
    second = (struct file){ 0, &e.dev.misc };
    assert(tbstream_dev_fops_open(NULL, &second) == 0 && qos_adds == 1);
    assert(tbstream_dev_fops_release(NULL, &second) == 0 && qos_live == 1);
    assert(tbstream_dev_fops_release(NULL, &e.file) == 0);
    assert(!qos_live && !e.dev.qos.active);

    /* The module parameter, clamped to 100 ms; 0 from the endpoint wins. */
    e.dev.cfg_latency_qos_us = -1;
    latency_qos_us = 1000000;
    assert(tbstream_dev_fops_open(NULL, &e.file) == 0);
    assert(qos_live == 1 && e.dev.qos.value == 100000);
    /* Suspend stops the rings and drops it; resume takes it again. */
    assert(tbstream_suspend(&e.svc[0].dev) == 0 && !qos_live);
    assert(tbstream_resume(&e.svc[0].dev) == 0 && qos_live == 1);
    assert(tbstream_dev_fops_release(NULL, &e.file) == 0 && !qos_live);
    e.dev.cfg_latency_qos_us = 0;
    assert(tbstream_dev_fops_open(NULL, &e.file) == 0 && e.dev.qos.value == 0);
    assert(tbstream_dev_fops_release(NULL, &e.file) == 0 && !qos_live);

    /* A failed open or resume holds none. */
    ring_fail_after = 0;
    assert(tbstream_dev_fops_open(NULL, &e.file) == -ENOMEM && !qos_live);
    assert(tbstream_dev_fops_open(NULL, &e.file) == 0 && qos_live == 1);
    assert(tbstream_suspend(&e.svc[0].dev) == 0 && !qos_live);
    ring_fail_after = 0;
    assert(tbstream_resume(&e.svc[0].dev) < 0 && !qos_live);
    close_stale_file(&e);

    /* Detach with the file open drops it. */
    assert(tbstream_dev_fops_open(NULL, &e.file) == 0 && qos_live == 1);
    unplug_and_remove(&e, 0);
    assert(e.dev.detached && !qos_live);
    assert_rings_gone(&e);
    close_stale_file(&e);
    latency_qos_us = -1;
    assert(!atomic_load(&stream_refs) && !atomic_load(&dev_refs));
}
