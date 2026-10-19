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
