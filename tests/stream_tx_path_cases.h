/* SPDX-License-Identifier: GPL-2.0-only */
/* Actual stream.c functions: 0051 TX memory-path experiments (tx_copy, tx_frame_bytes, tx_no_snoop). */

static void tx_path_reset(void)
{
    tx_copy = 0;
    tx_frame_bytes = 0;
    tx_no_snoop = false;
}

static void tx_path_finish(struct endpoint *e)
{
    while (complete_tx(e->dev.tx_ring.ring)) {}
    assert(tbstream_dev_fops_release(NULL, &e->file) == 0);
    assert_nothing_held();
    unplug_and_remove(e, 0);
    tx_path_reset();
}

/* Sizes of the TX frames posted and not completed yet, oldest first. */
static int tx_path_posted_sizes(struct tb_ring *r, u32 *sizes, int max)
{
    struct ring_frame *f;
    int n = 0;

    list_for_each_entry(f, &r->posted, list) {
        assert(n < max);
        sizes[n++] = f->size;
    }
    return n;
}

static void tx_frame_bytes_splits_writes(void)
{
    static unsigned char buf[2 * TB_MAX_FRAME_SIZE];
    u32 sizes[16];
    struct endpoint e;
    int before;

    for (unsigned int i = 0; i < sizeof(buf); ++i)
        buf[i] = (unsigned char)(i * 7 + 3);

    /* Default (0): 4096-byte frames, the existing path. */
    endpoint_init(&e, 0);
    assert(tbstream_dev_fops_open(NULL, &e.file) == 0);
    before = atomic_load(&tx_frames);
    assert(file_write(&e, buf, TB_MAX_FRAME_SIZE + 100) == TB_MAX_FRAME_SIZE + 100);
    assert(atomic_load(&tx_frames) == before + 2);
    assert(tx_path_posted_sizes(e.dev.tx_ring.ring, sizes, 16) == 2);
    assert(sizes[0] == 0 && sizes[1] == 100);   /* 0 means 4096 in a descriptor */
    tx_path_finish(&e);

    /* 1024: the same bytes in 1024-byte frames, contents unchanged. */
    tx_frame_bytes = 1024;
    endpoint_init(&e, 0);
    assert(tbstream_dev_fops_open(NULL, &e.file) == 0);
    tx_frame_bytes = 0;                          /* fixed at open: later changes do not apply */
    before = atomic_load(&tx_frames);
    assert(file_write(&e, buf, TB_MAX_FRAME_SIZE + 100) == TB_MAX_FRAME_SIZE + 100);
    assert(atomic_load(&tx_frames) == before + 5);
    assert(tx_path_posted_sizes(e.dev.tx_ring.ring, sizes, 16) == 5);
    for (int i = 0; i < 4; ++i)
        assert(sizes[i] == 1024);
    assert(sizes[4] == 100);
    for (int i = 0; i < 5; ++i) {
        struct tbstream_frame *sf = &e.dev.tx_ring.frames[i];

        assert(!memcmp(page_address(sf->page), buf + 1024 * i, i < 4 ? 1024 : 100));
    }
    tx_path_finish(&e);
}

static void tx_copy_nocache_same_bytes(void)
{
    static unsigned char buf[TB_MAX_FRAME_SIZE + 300];
    struct endpoint e;
    int before;

    for (unsigned int i = 0; i < sizeof(buf); ++i)
        buf[i] = (unsigned char)(i * 13 + 1);
    tx_copy = TBSTREAM_TX_COPY_NOCACHE;
    endpoint_init(&e, 0);
    assert(tbstream_dev_fops_open(NULL, &e.file) == 0);
    tx_copy = 0;
    before = atomic_load(&flushcache_copies);
    assert(file_write(&e, buf, sizeof(buf)) == sizeof(buf));
    assert(atomic_load(&flushcache_copies) == before + 2);
    assert(!memcmp(page_address(e.dev.tx_ring.frames[0].page), buf, TB_MAX_FRAME_SIZE));
    assert(!memcmp(page_address(e.dev.tx_ring.frames[1].page), buf + TB_MAX_FRAME_SIZE, 300));
    /* A fault inside the copy is reported like the cached copy does. */
    iov_budget = 1000;
    assert(file_write(&e, buf, sizeof(buf)) == -EFAULT);
    iov_budget = -1;
    tx_path_finish(&e);

    /* Default: the cached copy, no non-temporal path. */
    endpoint_init(&e, 0);
    assert(tbstream_dev_fops_open(NULL, &e.file) == 0);
    before = atomic_load(&flushcache_copies);
    assert(file_write(&e, buf, sizeof(buf)) == sizeof(buf));
    assert(atomic_load(&flushcache_copies) == before);
    tx_path_finish(&e);
}

static void tx_no_snoop_needs_uncached_copy(void)
{
    struct endpoint e;
    int warned;

    /* Refused with a cached copy: the controller could read stale lines. */
    tx_no_snoop = true;
    endpoint_init(&e, 0);
    warned = atomic_load(&warnings);
    assert(tbstream_dev_fops_open(NULL, &e.file) == -EINVAL);
    assert(atomic_load(&warnings) > warned);   /* ours, plus the open failure report */
    assert_nothing_held();
    unplug_and_remove(&e, 0);

    /* Allowed with non-temporal stores: only the TX ring asks for No Snoop. */
    tx_copy = TBSTREAM_TX_COPY_NOCACHE;
    endpoint_init(&e, 0);
    assert(tbstream_dev_fops_open(NULL, &e.file) == 0);
    assert(e.dev.tx_ring.ring->flags & RING_FLAG_NO_SNOOP);
    assert(!(e.dev.rx_ring.ring->flags & RING_FLAG_NO_SNOOP));
    tx_path_finish(&e);

    /* Off (default): neither ring. */
    endpoint_init(&e, 0);
    assert(tbstream_dev_fops_open(NULL, &e.file) == 0);
    assert(!(e.dev.tx_ring.ring->flags & RING_FLAG_NO_SNOOP));
    assert(!(e.dev.rx_ring.ring->flags & RING_FLAG_NO_SNOOP));
    tx_path_finish(&e);
}

static void tx_path_parameters_are_bounded(void)
{
    struct kernel_param kc = { &tx_copy }, kf = { &tx_frame_bytes };

    assert(tx_copy_set("3", &kc) == 0 && tx_copy == 3);
    assert(tx_copy_set("4", &kc) == -EINVAL && tx_copy == 3);
    assert(tx_copy_set("x", &kc) == -EINVAL && tx_copy == 3);
    assert(tx_copy_set("0", &kc) == 0 && tx_copy == 0);
    assert(tx_frame_bytes_set("1024", &kf) == 0 && tx_frame_bytes == 1024);
    assert(tx_frame_bytes_set("4096", &kf) == 0 && tx_frame_bytes == 4096);
    assert(tx_frame_bytes_set("4097", &kf) == -EINVAL && tx_frame_bytes == 4096);
    assert(tx_frame_bytes_set("192", &kf) == -EINVAL && tx_frame_bytes == 4096);
    assert(tx_frame_bytes_set("1000", &kf) == -EINVAL && tx_frame_bytes == 4096);
    assert(tx_frame_bytes_set("0", &kf) == 0 && tx_frame_bytes == 0);
    tx_path_reset();
}
