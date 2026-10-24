/* SPDX-License-Identifier: GPL-2.0-only */
/* Actual stream.c functions: 0050 paced RX descriptor polls (rx_poll_pace_ns). */
#define RX_PACE_FAR (ktime_get() + 1000000000LL)

static void rx_pace_finish(struct endpoint *e)
{
    while (complete_tx(e->dev.tx_ring.ring)) {}
    assert(tbstream_dev_fops_release(NULL, &e->file) == 0);
    assert_nothing_held();
    unplug_and_remove(e, 0);
}

static void rx_pace_skips_descriptor_polls(void)
{
    for (int full = 0; full < 2; ++full) {
        struct endpoint e;
        unsigned char buf[256] = { 0 };
        int before;

        endpoint_init(&e, full ? 2 : 0);
        e.dev.rx_busy_poll = !full;
        e.dev.rx_poll_pace_ns = 10000;
        e.dev.rx_poll_next = RX_PACE_FAR; /* stale value: open restarts it */
        assert(tbstream_dev_fops_open(NULL, &e.file) == 0);
        e.file.f_flags = O_NONBLOCK;
        assert(e.dev.rx_poll_next == 0);

        /* An empty poll starts the pace. */
        before = atomic_load(&poll_calls);
        assert(file_read(&e, buf, sizeof(buf)) == -EAGAIN);
        assert(atomic_load(&poll_calls) == before + 1);
        assert(e.dev.rx_poll_next > 0);

        /* Inside the pace the ring is not read, even with a completion waiting. */
        e.dev.rx_poll_next = RX_PACE_FAR;
        complete_rx(e.dev.rx_ring.ring, 64);
        before = atomic_load(&poll_calls);
        assert(file_read(&e, buf, sizeof(buf)) == -EAGAIN);
        assert(file_read(&e, buf, sizeof(buf)) == -EAGAIN);
        assert(atomic_load(&poll_calls) == before);

        /* Pace over: the waiting frame is delivered; a poll that reaped does not pace. */
        e.dev.rx_poll_next = ktime_get() - 1;
        assert(file_read(&e, buf, sizeof(buf)) == 64);
        assert(e.dev.rx_poll_next == 0);

        /* Frames reaped already stay readable while paced. */
        complete_rx(e.dev.rx_ring.ring, 30);
        complete_rx(e.dev.rx_ring.ring, 40);
        assert(file_read(&e, buf, 10) == 10);
        e.dev.rx_poll_next = RX_PACE_FAR;
        before = atomic_load(&poll_calls);
        assert(file_read(&e, buf, sizeof(buf)) == 60);
        assert(atomic_load(&poll_calls) == before);

        /* TX completions are never paced. */
        e.dev.rx_poll_next = RX_PACE_FAR;
        before = atomic_load(&poll_calls);
        assert(file_write(&e, buf, 64) == 64);
        assert(full ? atomic_load(&poll_calls) > before : atomic_load(&poll_calls) == before);
        rx_pace_finish(&e);
    }
}

static void rx_pace_zero_is_legacy(void)
{
    struct endpoint e;
    unsigned char buf[64] = { 0 };
    int before;

    endpoint_init(&e, 0);
    e.dev.rx_busy_poll = true;
    assert(tbstream_dev_fops_open(NULL, &e.file) == 0);
    e.file.f_flags = O_NONBLOCK;
    before = atomic_load(&poll_calls);
    for (int i = 0; i < 3; ++i)
        assert(file_read(&e, buf, sizeof(buf)) == -EAGAIN);
    assert(atomic_load(&poll_calls) == before + 3);
    assert(e.dev.rx_poll_next == 0);
    complete_rx(e.dev.rx_ring.ring, 64);
    assert(file_read(&e, buf, sizeof(buf)) == 64);
    rx_pace_finish(&e);
}

static void rx_pace_ignored_with_interrupts(void)
{
    struct endpoint e;
    unsigned char buf[64] = { 0 };
    int before;

    endpoint_init(&e, 0);
    e.dev.cfg_rx_napi = 1;
    e.dev.rx_poll_pace_ns = 10000;
    assert(tbstream_dev_fops_open(NULL, &e.file) == 0);
    e.file.f_flags = O_NONBLOCK;
    assert(e.dev.rx_napi);
    before = atomic_load(&poll_calls);
    assert(file_read(&e, buf, sizeof(buf)) == -EAGAIN);
    assert(file_read(&e, buf, sizeof(buf)) == -EAGAIN);
    assert(atomic_load(&poll_calls) > before); /* the NAPI reap still runs */
    assert(e.dev.rx_poll_next == 0);
    complete_rx(e.dev.rx_ring.ring, 64);
    assert(file_read(&e, buf, sizeof(buf)) == 64);
    rx_pace_finish(&e);
}
