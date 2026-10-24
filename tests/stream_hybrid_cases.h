/* SPDX-License-Identifier: GPL-2.0-only */
/* Actual stream/core functions above; timer/MMIO scheduling are explicit doubles. */
static int hint(struct tbstream_dev *s, unsigned int command, u64 cookie, u64 bytes)
{
    struct shs_rx_window h = { .cookie = cookie, .bytes = bytes };
    int ret;
    mutex_lock(&s->lock);
    ret = tbstream_dev_window_command(s, command, &h);
    mutex_unlock(&s->lock);
    return ret;
}

static void hybrid_policy_and_lifetime(void)
{
    struct host h;
    host_init(&h, 64);
    struct tbstream_dev *s = &h.dev[0];
    assert(sizeof(struct shs_rx_window) == 24);
    assert(start(s) == 0);
    assert(hint(s, SHS_RX_WINDOW_BEGIN, 1, 20000) == -EOPNOTSUPP);
    stop(s);
    s->rx_window_bytes = 32768;
    assert(start(s) == 0 && s->rx_napi);
    assert(!(s->rx_ring.ring->flags & RING_FLAG_NO_INTERRUPT));
    assert(!(s->tx_ring.ring->flags & RING_FLAG_NO_INTERRUPT));
    assert(hint(s, SHS_RX_WINDOW_BEGIN, 0, 20) == -EINVAL);
    assert(hint(s, SHS_RX_WINDOW_BEGIN, 1, 0) == -EINVAL);
    assert(hint(s, SHS_RX_WINDOW_BEGIN, 1, 32769) == -E2BIG);
    assert(!s->window_active && !s->rx_ring.ring->poll_window);
    assert(hint(s, SHS_RX_WINDOW_BEGIN, 1, 32768) == 0);
    assert(s->window_active && s->rx_ring.ring->masked && s->window_timer.armed);
    assert(!s->tx_ring.ring->masked);
    tb_ring_poll_complete(s->rx_ring.ring);
    assert(s->rx_ring.ring->masked);
    assert(hint(s, SHS_RX_WINDOW_BEGIN, 2, 20) == -EBUSY);
    assert(hint(s, SHS_RX_WINDOW_END, 2, 0) == -ESTALE);
    /* An early timer callback must restart, not abandon the IRQ lease. */
    assert(tbstream_dev_window_expire(&s->window_timer) == HRTIMER_RESTART);
    assert(s->window_active && s->rx_ring.ring->masked);
    complete_rx(s->rx_ring.ring, 128);
    assert(hint(s, SHS_RX_WINDOW_END, 1, 0) == 0);
    assert(!s->window_active && !s->rx_ring.ring->masked);
    assert(hint(s, SHS_RX_WINDOW_END, 1, 0) == 0);
    /* Completion remains owned once, across mask/rearm. */
    mutex_lock(&s->lock); tbstream_dev_napi_rx(s); mutex_unlock(&s->lock);
    assert(tbstream_ring_available(&s->rx_ring));
    assert(hint(s, SHS_RX_WINDOW_BEGIN, 2, 20) == 0);
    s->window_deadline = ktime_get() - 1;
    assert(tbstream_dev_window_expire(&s->window_timer) == HRTIMER_NORESTART);
    assert(!s->window_active && !s->rx_ring.ring->masked);
    assert(hint(s, SHS_RX_WINDOW_END, 1, 0) == -ESTALE);
    assert(hint(s, SHS_RX_WINDOW_BEGIN, 3, 20) == 0);
    stop(s); /* Cancel before ring_free, even if END was omitted. */
    assert(!s->window_active && !s->window_timer.armed);
    assert(hint(s, SHS_RX_WINDOW_BEGIN, 4, 20) == -ENODEV);
    assert_nothing_held();
}

static void hybrid_core_guards(void)
{
    struct host h; host_init(&h, 64);
    struct tbstream_dev *s = &h.dev[0]; s->rx_window_bytes = 20000;
    assert(start(s) == 0);
    struct tb_ring *r = s->rx_ring.ring;
    assert(tb_ring_poll_begin(s->tx_ring.ring) == -EOPNOTSUPP);
    r->running = false; assert(tb_ring_poll_begin(r) == -ENODEV); r->running = true;
    h.nhi.going_away = true; assert(tb_ring_poll_begin(r) == -ENODEV); h.nhi.going_away = false;
    r->flags |= RING_FLAG_NO_INTERRUPT; assert(tb_ring_poll_begin(r) == -EOPNOTSUPP); r->flags &= ~RING_FLAG_NO_INTERRUPT;
    assert(tb_ring_poll_begin(r) == 0);
    assert(tb_ring_poll_begin(r) == -EBUSY);
    /* Stop/gone must never unmask hardware from a late END. */
    r->running = false; tb_ring_poll_end(r); assert(r->masked && !r->poll_window); r->running = true;
    assert(tb_ring_poll_begin(r) == 0);
    h.nhi.going_away = true; tb_ring_poll_end(r); assert(r->masked && !r->poll_window); h.nhi.going_away = false;
    stop(s); assert_nothing_held();
}

static void *expire_worker(void *p)
{
    struct tbstream_dev *s = p;
    tbstream_dev_window_expire(&s->window_timer);
    return NULL;
}
static void hybrid_expire_end_race(void)
{
    struct host h; host_init(&h, 64);
    struct tbstream_dev *s = &h.dev[0]; s->rx_window_bytes = 32768;
    assert(start(s) == 0);
    for (unsigned i = 1; i <= 500; ++i) {
        pthread_t t;
        assert(hint(s, SHS_RX_WINDOW_BEGIN, i, 20) == 0);
        s->window_deadline = ktime_get() - 1;
        assert(!pthread_create(&t, NULL, expire_worker, s));
        assert(hint(s, SHS_RX_WINDOW_END, i, 0) == 0);
        assert(!pthread_join(t, NULL));
        assert(!s->window_active && !s->rx_ring.ring->poll_window && !s->rx_ring.ring->masked);
    }
    stop(s); assert_nothing_held();
}

static void hybrid_config_and_exclusive_open(void)
{
    struct host h; host_init(&h, 64);
    struct tbstream_dev *s = &h.dev[0];
    struct config_item *item = &s->group.cg_item;
    assert(tbstream_dev_rx_window_bytes_store(item, "1048577", 7) == -EINVAL);
    assert(tbstream_dev_rx_window_us_store(item, "0", 1) == -EINVAL);
    assert(tbstream_dev_rx_window_us_store(item, "10001", 5) == -EINVAL);
    assert(tbstream_dev_rx_window_us_store(item, "10000", 5) == 5);
    s->busy_poll = true;
    assert(tbstream_dev_rx_window_bytes_store(item, "20000", 5) == -EOPNOTSUPP);
    s->busy_poll = false; s->rx_busy_poll = true;
    assert(tbstream_dev_rx_window_bytes_store(item, "20000", 5) == -EOPNOTSUPP);
    s->rx_busy_poll = false;
    /* Window vs 0048 batch budgets: EBUSY both ways; storing 0 is always allowed. */
    assert(tbstream_dev_rx_repost_batch_store(item, "64", 2) == 2 && s->rx_repost_batch == 64);
    assert(tbstream_dev_rx_window_bytes_store(item, "20000", 5) == -EBUSY && !s->rx_window_bytes);
    assert(tbstream_dev_rx_window_bytes_store(item, "0", 1) == 1 && !s->rx_window_bytes);
    assert(tbstream_dev_rx_repost_batch_store(item, "0", 1) == 1 && !s->rx_repost_batch);
    assert(tbstream_dev_tx_submit_batch_store(item, "64", 2) == 2 && s->tx_submit_batch == 64);
    assert(tbstream_dev_rx_window_bytes_store(item, "20000", 5) == -EBUSY && !s->rx_window_bytes);
    assert(tbstream_dev_rx_window_bytes_store(item, "0", 1) == 1 && !s->rx_window_bytes);
    assert(tbstream_dev_tx_submit_batch_store(item, "0", 1) == 1 && !s->tx_submit_batch);
    assert(tbstream_dev_rx_window_bytes_store(item, "20000", 5) == 5 && s->rx_window_bytes == 20000);
    assert(tbstream_dev_rx_repost_batch_store(item, "64", 2) == -EBUSY && !s->rx_repost_batch);
    assert(tbstream_dev_tx_submit_batch_store(item, "64", 2) == -EBUSY && !s->tx_submit_batch);
    assert(tbstream_dev_rx_repost_batch_store(item, "0", 1) == 1 && !s->rx_repost_batch);
    assert(tbstream_dev_tx_submit_batch_store(item, "0", 1) == 1 && !s->tx_submit_batch);
    assert(tbstream_dev_rx_window_bytes_store(item, "0", 1) == 1 && !s->rx_window_bytes);
    assert(tbstream_dev_rx_repost_batch_store(item, "64", 2) == 2 && s->rx_repost_batch == 64);
    assert(tbstream_dev_tx_submit_batch_store(item, "64", 2) == 2 && s->tx_submit_batch == 64);
    assert(tbstream_dev_rx_repost_batch_store(item, "0", 1) == 1 && !s->rx_repost_batch);
    assert(tbstream_dev_tx_submit_batch_store(item, "0", 1) == 1 && !s->tx_submit_batch);
    assert(tbstream_dev_rx_window_bytes_store(item, "20000", 5) == 5 && s->rx_window_bytes == 20000);
    assert(tbstream_dev_rx_repost_batch_store(item, "64", 2) == -EBUSY);
    assert(tbstream_dev_tx_submit_batch_store(item, "64", 2) == -EBUSY);
    assert(tbstream_dev_busy_poll_store(item, "1", 1) == -EOPNOTSUPP);
    assert(tbstream_dev_rx_busy_poll_store(item, "1", 1) == -EOPNOTSUPP);
    struct file file = { .f_flags = O_NONBLOCK, .private_data = &s->misc };
    assert(tbstream_dev_fops_open(NULL, &file) == 0);
    assert(tbstream_dev_fops_open(NULL, &file) == -EBUSY);
    assert(s->users == 1);
    assert(tbstream_dev_rx_window_bytes_store(item, "0", 1) == -EBUSY);
    assert(tbstream_dev_rx_window_us_store(item, "1", 1) == -EBUSY);
    assert(hint(s, SHS_RX_WINDOW_BEGIN, 1, 20000) == 0);
    stop(s); s->detached = true;
    assert(tbstream_dev_fops_release(NULL, &file) == 0);
    assert(!s->users);
    assert_nothing_held();
}

static void hybrid_ioctl_faults(void)
{
    struct host h; host_init(&h, 64);
    struct tbstream_dev *s = &h.dev[0];
    struct shs_rx_window hint = { .cookie=1, .bytes=20 };
    struct file f = { .private_data=&s->misc };
    assert(tbstream_dev_fops_ioctl(&f, SHS_RX_WINDOW_BEGIN, (unsigned long)&hint)==-EOPNOTSUPP);
    f.f_flags=O_NONBLOCK; user_copy_fault=true;
    assert(tbstream_dev_fops_ioctl(&f, SHS_RX_WINDOW_BEGIN, 1)==-EFAULT);
    user_copy_fault=false;
    assert(tbstream_dev_fops_ioctl(&f, 0, 0)==-ENOTTY);
    s->rx_window_bytes=32768; assert(start(s)==0);
    hint.flags=1;
    assert(tbstream_dev_fops_ioctl(&f, SHS_RX_WINDOW_BEGIN, (unsigned long)&hint)==-EINVAL);
    hint.flags=0; hint.reserved=1;
    assert(tbstream_dev_fops_ioctl(&f, SHS_RX_WINDOW_BEGIN, (unsigned long)&hint)==-EINVAL);
    hint.reserved=0;
    assert(tbstream_dev_fops_ioctl(&f, SHS_RX_WINDOW_BEGIN, (unsigned long)&hint)==0);
    assert(s->window_active);
    assert(tbstream_dev_fops_ioctl(&f, SHS_RX_WINDOW_END, (unsigned long)&hint)==0);
    stop(s); assert_nothing_held();
}
