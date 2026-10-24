/* SPDX-License-Identifier: GPL-2.0-only */
/* Actual stream.c functions; mocks model completion delivery, not hardware. */
static void rx_only_poll_keeps_tx_interrupts(void)
{
    for (int full = 0; full < 2; ++full) {
        for (int rx = 0; rx < 2; ++rx) {
            struct host h;
            struct tbstream_dev *s = &h.dev[0];
            struct file f;
            int before;

            host_init(&h, 8);
            s->cfg_rx_napi = 1;
            s->busy_poll = full;
            s->rx_busy_poll = rx;
            assert(start(s) == 0);
            assert(!!(s->tx_ring.ring->flags & RING_FLAG_NO_INTERRUPT) == full);
            assert(!!(s->rx_ring.ring->flags & RING_FLAG_NO_INTERRUPT) == (full || rx));
            assert(s->rx_napi == !(full || rx));
            assert(!!s->rx_ring.ring->start_poll == !(full || rx));
            before = atomic_load(&poll_calls);
            tbstream_dev_poll_ring(s, &s->tx_ring);
            assert(atomic_load(&poll_calls) == before + full);
            before = atomic_load(&poll_calls);
            tbstream_dev_poll_ring(s, &s->rx_ring);
            assert(atomic_load(&poll_calls) == before + !!(full || rx));
            f.private_data = &s->misc;
            if (full || rx) assert(tbstream_dev_fops_poll(&f, NULL) == EPOLLERR);
            stop(s);
            assert_nothing_held();
        }
    }
}

static void rx_only_poll_reads_and_errors(void)
{
    struct endpoint e;
    unsigned char buf[256] = { 0 };
    int before;

    endpoint_init(&e, 0);
    e.dev.rx_busy_poll = true;
    e.dev.cfg_rx_napi = 1; /* RX-only explicitly wins over NAPI. */
    assert(tbstream_dev_fops_open(NULL, &e.file) == 0);
    e.file.f_flags = O_NONBLOCK;
    assert(!e.dev.rx_napi && !e.dev.rx_ring.ring->start_poll);
    assert(file_read(&e, buf, sizeof(buf)) == -EAGAIN);
    complete_rx(e.dev.rx_ring.ring, 76);
    assert(file_read(&e, buf, 17) == 17);
    assert(file_read(&e, buf, sizeof(buf)) == 59);
    assert(file_read(&e, buf, sizeof(buf)) == -EAGAIN);
    assert(file_write(&e, buf, 64) == 64);
    before = atomic_load(&poll_calls);
    assert(complete_tx(e.dev.tx_ring.ring)); /* interrupt callback restores credit */
    assert(file_write(&e, buf, 64) == 64);
    assert(atomic_load(&poll_calls) == before); /* never inline-poll TX */
    while (complete_tx(e.dev.tx_ring.ring)) {}
    complete_rx_flags(e.dev.rx_ring.ring, 64, RING_DESC_CRC_ERROR);
    assert(file_read(&e, buf, sizeof(buf)) == -EIO);
    assert(tbstream_dev_fops_poll(&e.file, NULL) == EPOLLERR);
    assert(tbstream_dev_fops_release(NULL, &e.file) == 0);
    assert_nothing_held();
    unplug_and_remove(&e, 0);
}
