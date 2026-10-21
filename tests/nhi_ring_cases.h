/* SPDX-License-Identifier: GPL-2.0-only */
/* Included after the extracted nhi.c ring functions. */

#define FIX_SIZE 8
#define FIX_FRAMES 12

struct fixture {
    struct tb_nhi nhi;
    struct tb_ring ring;
    struct ring_desc desc[FIX_SIZE];
    struct ring_frame frames[FIX_FRAMES];
};

static struct tb_ring_stamps seen[FIX_FRAMES];
static int calls, canceled_calls, seen_count;
static u64 callback_cost = 0;

static void frame_cb(struct tb_ring *ring, struct ring_frame *frame, bool canceled)
{
    (void)frame;
    assert(!ring->lock.held); /* callbacks run outside the ring lock */
    ++calls;
    if (canceled) {
        ++canceled_calls;
        return;
    }
    seen[seen_count++] = ring->cb_stamps;
    fake_now += callback_cost;
}

static void fixture_init(struct fixture *f, bool tx)
{
    memset(f, 0, sizeof(*f));
    spin_lock_init(&f->nhi.lock);
    spin_lock_init(&f->ring.lock);
    f->ring.nhi = &f->nhi;
    f->ring.size = FIX_SIZE;
    f->ring.hop = 2;
    f->ring.descriptors = f->desc;
    f->ring.is_tx = tx;
    f->ring.running = true;
    INIT_LIST_HEAD(&f->ring.queue);
    INIT_LIST_HEAD(&f->ring.in_flight);
    for (int i = 0; i < FIX_FRAMES; ++i) {
        f->frames[i].callback = frame_cb;
        f->frames[i].buffer_phy = 0x1000 * (i + 1);
        f->frames[i].size = 64;
        INIT_LIST_HEAD(&f->frames[i].list);
    }
    calls = canceled_calls = seen_count = 0;
    works_queued = doorbells = masks = 0;
    callback_cost = 0;
    fake_now = 1000;
}

/* Queue frames [first, first + n) and post as many as the ring takes. */
static void post(struct fixture *f, int first, int n)
{
    spin_lock(&f->ring.lock);
    for (int i = first; i < first + n; ++i)
        list_add_tail(&f->frames[i].list, &f->ring.queue);
    ring_write_descriptors(&f->ring);
    spin_unlock(&f->ring.lock);
}

/* The controller completes the next @n posted descriptors. */
static void hw_complete(struct fixture *f, int n)
{
    int at = f->ring.tail;

    for (int i = 0; i < n; ++i) {
        assert(at != f->ring.head);
        f->desc[at].flags |= RING_DESC_COMPLETED;
        f->desc[at].length = 64;
        f->desc[at].eof = 2;
        f->desc[at].sof = 1;
        at = (at + 1) % f->ring.size;
    }
}

static int in_flight(struct fixture *f)
{
    int n = 0;

    for (struct list_head *p = f->ring.in_flight.next; p != &f->ring.in_flight; p = p->next)
        ++n;
    return n;
}

/* 0040: the interrupt is timed and counted, with or without start_poll. */
static void irq_is_stamped(void)
{
    struct fixture f;

    fixture_init(&f, false);
    fake_now = 5000;
    spin_lock(&f.ring.lock);
    __ring_interrupt(&f.ring);
    spin_unlock(&f.ring.lock);
    assert(f.ring.irqs == 1 && f.ring.last_irq_ns == 5001 && works_queued == 1);
    /* A stopped ring ignores it entirely. */
    f.ring.running = false;
    spin_lock(&f.ring.lock);
    __ring_interrupt(&f.ring);
    spin_unlock(&f.ring.lock);
    assert(f.ring.irqs == 1 && works_queued == 1);
}

static int polls_started;
static void start_poll(void *data) { (void)data; ++polls_started; }

static void irq_is_stamped_with_start_poll(void)
{
    struct fixture f;

    fixture_init(&f, false);
    f.ring.start_poll = start_poll;
    spin_lock(&f.ring.lock);
    __ring_interrupt(&f.ring);
    spin_unlock(&f.ring.lock);
    assert(f.ring.irqs == 1 && f.ring.last_irq_ns && polls_started == 1 && masks == 1 && !works_queued);
}

/* 0040: a collection by ring_work records its IRQ, its start and the moment
 * it took the completed descriptors; the callbacks see them; its duration
 * runs to the last callback. Queued frames are posted as slots free up. */
static void work_collection_is_stamped(void)
{
    struct fixture f;

    fixture_init(&f, true);
    post(&f, 0, 10);                     /* 7 fit (one slot stays free), 3 queued */
    assert(in_flight(&f) == 7 && doorbells == 1);
    hw_complete(&f, 2);
    spin_lock(&f.ring.lock);
    __ring_interrupt(&f.ring);           /* IRQ at t = irq */
    spin_unlock(&f.ring.lock);
    u64 irq = f.ring.last_irq_ns;
    fake_now += 300000;                  /* the worker starts 300 us later */
    callback_cost = 1000;
    ring_work(&f.ring.work);
    assert(calls == 2 && !canceled_calls && seen_count == 2);
    for (int i = 0; i < 2; ++i) {
        assert(seen[i].irq_ns == irq);
        assert(seen[i].start_ns >= irq + 300000);
        assert(seen[i].reap_ns > seen[i].start_ns);
    }
    assert(f.ring.collections == 1);
    assert(f.ring.collect_ns_total >= 2000 && f.ring.collect_ns_max == f.ring.collect_ns_total);
    assert(in_flight(&f) == 7 && doorbells == 2); /* two queued frames refilled */
    /* Nothing completed: no collection, the stamps stay those of the last one. */
    struct tb_ring_stamps before = f.ring.cb_stamps;
    ring_work(&f.ring.work);
    assert(f.ring.collections == 1 && calls == 2);
    assert(!memcmp(&before, &f.ring.cb_stamps, sizeof(before)));
}

/* 0040: the cancellation run of a stopped ring is not a timed collection. */
static void cancel_is_not_a_collection(void)
{
    struct fixture f;

    fixture_init(&f, true);
    post(&f, 0, 9);
    hw_complete(&f, 3);
    f.ring.running = false;
    ring_work(&f.ring.work);
    assert(calls == 9 && canceled_calls == 9 && !f.ring.collections && !f.ring.collect_ns_total);
}

/* 0041: a polled ring (no interrupt) is completed by tb_ring_collect(): in
 * order, at most @budget frames, freed slots refilled from the queue (the
 * descriptors carry no interrupt flag), callbacks outside the lock, never
 * as canceled; a collection is timed like the ring work's, without IRQ. */
static void collect_in_order_with_budget_and_refill(void)
{
    struct fixture f;

    fixture_init(&f, true);
    f.ring.flags = RING_FLAG_NO_INTERRUPT;
    post(&f, 0, 10);
    assert(in_flight(&f) == 7 && doorbells == 1);
    for (int i = 0; i < 7; ++i)
        assert(!(f.desc[i].flags & RING_DESC_INTERRUPT) && (f.desc[i].flags & RING_DESC_POSTED));
    hw_complete(&f, 4);
    assert(tb_ring_collect(&f.ring, 2) == 2);
    assert(calls == 2 && !canceled_calls && seen_count == 2 && f.ring.polls == 1 && f.ring.collections == 1);
    assert(seen[0].irq_ns == 0 && seen[0].reap_ns > seen[0].start_ns);
    assert(list_empty(&f.frames[0].list) && list_empty(&f.frames[1].list));
    assert(in_flight(&f) == 7 && doorbells == 2); /* 2 queued frames took the freed slots */
    assert(tb_ring_collect(&f.ring, 64) == 2 && calls == 4);
    assert(in_flight(&f) == 6 && doorbells == 3 && list_empty(&f.ring.queue));
    /* Nothing completed: no callback, no collection, no doorbell. */
    assert(tb_ring_collect(&f.ring, 64) == 0 && calls == 4 && f.ring.collections == 2);
    assert(f.ring.polls == 3 && doorbells == 3 && !warnings);
}

/* An interrupt ring is never collected this way (its ring work does it). */
static void collect_refuses_interrupt_ring(void)
{
    struct fixture f;

    fixture_init(&f, true);
    post(&f, 0, 3);
    hw_complete(&f, 3);
    warnings = 0;
    assert(tb_ring_collect(&f.ring, 64) == 0 && !calls && warnings == 1 && in_flight(&f) == 3);
    warnings = 0;
}

/* Collected frames are completed once; the stop cancels the rest once, also
 * the ones the controller had finished but nobody collected. */
static void collect_then_stop_exactly_once(void)
{
    struct fixture f;

    fixture_init(&f, true);
    f.ring.flags = RING_FLAG_NO_INTERRUPT;
    post(&f, 0, 5);
    hw_complete(&f, 3);
    assert(tb_ring_collect(&f.ring, 1) == 1 && calls == 1);
    f.ring.running = false;              /* tb_ring_stop() */
    ring_work(&f.ring.work);             /* its cancellation run */
    assert(calls == 5 && canceled_calls == 4);
    assert(tb_ring_collect(&f.ring, 64) == 0 && calls == 5);
}

/* RX: size, PDFs and flags (CRC/overrun) come from the descriptor. */
static void collect_rx_fields(void)
{
    struct fixture f;

    fixture_init(&f, false);
    f.ring.flags = RING_FLAG_NO_INTERRUPT;
    post(&f, 0, 2);
    hw_complete(&f, 2);
    f.desc[1].length = 100;
    f.desc[1].eof = 3;
    f.desc[1].flags |= RING_DESC_CRC_ERROR;
    assert(tb_ring_collect(&f.ring, 64) == 2);
    assert(f.frames[0].size == 64 && f.frames[0].eof == 2 && f.frames[0].sof == 1);
    assert(f.frames[1].size == 100 && f.frames[1].eof == 3 && (f.frames[1].flags & RING_DESC_CRC_ERROR));
}

/* 0043: the diagnostic snapshot reads head, tail, the queue and whether the
 * front descriptor completed, and changes nothing (no reap, refill,
 * doorbell or callback). */
static void snapshot_reads_without_changing(void)
{
    struct tb_ring_snapshot s;
    struct fixture f;

    fixture_init(&f, true);
    post(&f, 0, 10);
    tb_ring_snapshot(&f.ring, &s);
    assert(s.head == 7 && s.tail == 0 && s.queued == 3 && !s.front_completed && s.running);
    hw_complete(&f, 2);
    tb_ring_snapshot(&f.ring, &s);
    assert(s.front_completed && s.head == 7 && s.tail == 0 && s.queued == 3);
    assert(!calls && doorbells == 1 && in_flight(&f) == 7 && !f.ring.lock.held);
    ring_work(&f.ring.work);
    tb_ring_snapshot(&f.ring, &s);
    assert(calls == 2 && s.tail == 2 && s.head == 1 && s.queued == 1 && !s.front_completed);
    /* An empty ring has no front descriptor, even a stale completed one. */
    fixture_init(&f, false);
    f.desc[0].flags = RING_DESC_COMPLETED;
    tb_ring_snapshot(&f.ring, &s);
    assert(!s.head && !s.tail && !s.queued && !s.front_completed);
    f.ring.running = false;
    tb_ring_snapshot(&f.ring, &s);
    assert(!s.running && !calls && !doorbells);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    if (!strcmp(argv[1], "irq_is_stamped"))
        irq_is_stamped();
    else if (!strcmp(argv[1], "irq_is_stamped_with_start_poll"))
        irq_is_stamped_with_start_poll();
    else if (!strcmp(argv[1], "work_collection_is_stamped"))
        work_collection_is_stamped();
    else if (!strcmp(argv[1], "cancel_is_not_a_collection"))
        cancel_is_not_a_collection();
    else if (!strcmp(argv[1], "collect_in_order_with_budget_and_refill"))
        collect_in_order_with_budget_and_refill();
    else if (!strcmp(argv[1], "collect_refuses_interrupt_ring"))
        collect_refuses_interrupt_ring();
    else if (!strcmp(argv[1], "snapshot_reads_without_changing"))
        snapshot_reads_without_changing();
    else if (!strcmp(argv[1], "collect_then_stop_exactly_once"))
        collect_then_stop_exactly_once();
    else if (!strcmp(argv[1], "collect_rx_fields"))
        collect_rx_fields();
    else
        return 2;
    printf("PASS %s\n", argv[1]);
    return 0;
}
