/* SPDX-License-Identifier: GPL-2.0-only */
/* Actual host functions with atomic enqueue ownership/fault injection. */
static void batch_reset_counts(void)
{
    memset(batch_calls, 0, sizeof(batch_calls));
    memset(batch_frames, 0, sizeof(batch_frames));
    memset(batch_max, 0, sizeof(batch_max));
}

static void batch_finish(struct endpoint *e)
{
    while (complete_tx(e->dev.tx_ring.ring)) {}
    assert(tbstream_dev_fops_release(NULL, &e->file) == 0);
    assert_nothing_held();
    unplug_and_remove(e, 0);
}

static void host_batch_tx_limits(void)
{
    const unsigned int budgets[] = { 1, 2, 4, 64 };
    unsigned char data[5 * TB_MAX_FRAME_SIZE + 17];

    for (size_t i = 0; i < sizeof(data); ++i) data[i] = i * 13;
    for (unsigned int b = 0; b < sizeof(budgets) / sizeof(budgets[0]); ++b) {
        struct endpoint e;
        struct list_head *p;
        size_t offset = 0;

        endpoint_init(&e, 1);
        e.dev.tx_submit_batch = budgets[b];
        assert(tbstream_dev_fops_open(NULL, &e.file) == 0);
        batch_reset_counts();
        assert(file_write(&e, data, sizeof(data)) == sizeof(data));
        assert(batch_frames[1] == 6 && batch_calls[1] == (6 + budgets[b] - 1) / budgets[b]);
        assert(batch_max[1] <= budgets[b]);
        for (p = e.dev.tx_ring.ring->posted.next; p != &e.dev.tx_ring.ring->posted; p = p->next) {
            struct ring_frame *f = container_of(p, struct ring_frame, list);
            struct tbstream_frame *sf = container_of(f, struct tbstream_frame, frame);
            size_t n = tb_ring_frame_size(f);
            assert(f->eof == TBSTREAM_DATA && !memcmp(page_address(sf->page), data + offset, n));
            offset += n;
        }
        assert(offset == sizeof(data));
        batch_finish(&e);
    }
}

static void host_batch_tx_faults(void)
{
    struct endpoint e;
    unsigned char data[5 * TB_MAX_FRAME_SIZE] = { 42 };
    struct kiocb k;
    struct iov_iter it;
    unsigned int cons;

    endpoint_init(&e, 1);
    e.dev.tx_submit_batch = 2;
    assert(tbstream_dev_fops_open(NULL, &e.file) == 0);
    e.file.f_flags = O_NONBLOCK;
    k = (struct kiocb){ &e.file, 0 };
    batch_reset_counts();
    iov_budget = TB_MAX_FRAME_SIZE + 11;
    it = (struct iov_iter){ data, sizeof(data) };
    assert(tbstream_dev_fops_write_iter(&k, &it) == TB_MAX_FRAME_SIZE);
    assert(it.buf == data + TB_MAX_FRAME_SIZE && it.count == sizeof(data) - TB_MAX_FRAME_SIZE);
    assert(batch_frames[1] == 1);
    iov_budget = -1;
    while (complete_tx(e.dev.tx_ring.ring)) {}

    /* The first batch commits, the next batch fails atomically. */
    cons = e.dev.tx_ring.cons;
    batch_fail_after = 1;
    it = (struct iov_iter){ data, sizeof(data) };
    assert(tbstream_dev_fops_write_iter(&k, &it) == 2 * TB_MAX_FRAME_SIZE);
    assert(it.buf == data + 2 * TB_MAX_FRAME_SIZE);
    assert(e.dev.tx_ring.cons == cons + 2 && batch_fail_after == -1);
    while (complete_tx(e.dev.tx_ring.ring)) {}
    cons = e.dev.tx_ring.cons;
    batch_fail_after = 0;
    it = (struct iov_iter){ data, sizeof(data) };
    assert(tbstream_dev_fops_write_iter(&k, &it) == -ESHUTDOWN);
    assert(it.buf == data && it.count == sizeof(data) && e.dev.tx_ring.cons == cons);
    assert(list_empty(&e.dev.tx_ring.ring->posted));
    /* Failed local lists are detached; the same slots can be submitted again. */
    assert(file_write(&e, data, sizeof(data)) == sizeof(data));
    batch_finish(&e);
}

static void host_batch_rx_flush(void)
{
    const unsigned int budgets[] = { 1, 2, 4, 64 };
    unsigned char data[4096];

    for (unsigned int b = 0; b < sizeof(budgets) / sizeof(budgets[0]); ++b) {
        for (int mode = 1; mode <= 2; ++mode) {
            struct endpoint e;

            endpoint_init(&e, mode);
            e.dev.rx_repost_batch = budgets[b];
            assert(tbstream_dev_fops_open(NULL, &e.file) == 0);
            e.file.f_flags = O_NONBLOCK;
            batch_reset_counts();
            for (int i = 0; i < 5; ++i) complete_rx(e.dev.rx_ring.ring, 64);
            assert(file_read(&e, data, sizeof(data)) == 320);
            assert(batch_frames[0] == 5 && batch_calls[0] == (5 + budgets[b] - 1) / budgets[b]);
            assert(batch_max[0] <= budgets[b]);
            assert(file_read(&e, data, sizeof(data)) == -EAGAIN);
            /* No waiting for a full batch, even for a short read. */
            complete_rx(e.dev.rx_ring.ring, 64);
            assert(file_read(&e, data, 17) == 17);
            assert(batch_frames[0] == 5);
            assert(file_read(&e, data, sizeof(data)) == 47);
            assert(batch_frames[0] == 6);
            batch_finish(&e);
        }
    }
}

static void host_batch_rx_faults(void)
{
    struct endpoint e;
    unsigned char data[512];

    endpoint_init(&e, 1);
    e.dev.rx_repost_batch = 64;
    assert(tbstream_dev_fops_open(NULL, &e.file) == 0);
    e.file.f_flags = O_NONBLOCK;
    batch_reset_counts();
    complete_rx(e.dev.rx_ring.ring, 64);
    complete_rx(e.dev.rx_ring.ring, 64);
    iov_budget = 74;
    assert(file_read(&e, data, sizeof(data)) == 74);
    iov_budget = -1;
    assert(batch_frames[0] == 1);
    assert(file_read(&e, data, sizeof(data)) == 54);
    assert(batch_frames[0] == 2);
    complete_rx(e.dev.rx_ring.ring, 64);
    complete_rx_flags(e.dev.rx_ring.ring, 64, RING_DESC_CRC_ERROR);
    assert(file_read(&e, data, sizeof(data)) == 64);
    assert(batch_frames[0] == 3 && file_read(&e, data, sizeof(data)) == -EIO);
    batch_finish(&e);

    endpoint_init(&e, 1);
    e.dev.rx_repost_batch = 2;
    assert(tbstream_dev_fops_open(NULL, &e.file) == 0);
    complete_rx(e.dev.rx_ring.ring, 64);
    batch_fail_after = 0;
    assert(file_read(&e, data, sizeof(data)) == 64); /* delivered prefix stays */
    assert(e.dev.rx_failed && file_read(&e, data, sizeof(data)) == -EIO);
    batch_finish(&e); /* ASAN validates no stack-list references on cancellation */

    endpoint_init(&e, 1);
    e.dev.rx_repost_batch = 64;
    assert(tbstream_dev_fops_open(NULL, &e.file) == 0);
    batch_reset_counts();
    complete_rx_frame(e.dev.rx_ring.ring, 256, TBSTREAM_CLOSE, 0);
    assert(file_read(&e, data, sizeof(data)) == 0 && e.dev.closed);
    assert(batch_frames[0] == 1);
    batch_finish(&e);
}
