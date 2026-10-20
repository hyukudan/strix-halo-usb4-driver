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
    else
        return 2;
    printf("PASS %s\n", argv[1]);
    return 0;
}
