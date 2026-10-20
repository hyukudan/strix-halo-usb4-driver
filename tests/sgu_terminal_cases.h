/* SPDX-License-Identifier: GPL-2.0-only */
/* Kernel-generated (TX) and kernel-validated (RX) terminal frames. */
static int prepare_header(struct fixture *f, unsigned d, unsigned slot, u64 handle, u64 seq,
                          u64 offset, u64 bytes, u32 terminal_bytes, u64 flags)
{
    struct sgu_prepare_slot p = {0};
    p.operation = (struct sgu_prepare){ .fence = f->sdev.gpu_context->identity, .handle = handle,
        .sequence = seq, .pool_offset = offset, .payload_bytes = bytes,
        .terminal_bytes = terminal_bytes, .direction = d };
    p.operation.flags = flags;
    p.slot = slot;
    return tbstream_gpu_ioctl(&f->file, SGU_IOC_PREPARE_SLOT, (unsigned long)&p);
}

static struct sgu_hw_frame *queued_frame(struct tb_ring *ring, unsigned index)
{
    struct list_head *p = ring->queue.next;
    while (index--)
        p = p->next;
    return container_of(container_of(p, struct ring_frame, list), struct sgu_hw_frame, frame);
}

static struct sgu_event read_event(struct fixture *f)
{
    struct sgu_event e = { .fence = f->sdev.gpu_context->identity };
    assert(tbstream_gpu_ioctl(&f->file, SGU_IOC_EVENT, (unsigned long)&e) == 0);
    return e;
}

/* The peer's terminal arrives: write it where the controller would. */
static void deliver_rx_terminal(struct fixture *f, unsigned slot, const void *bytes, u32 size)
{
    struct tbstream_gpu_context *ctx = f->sdev.gpu_context;
    struct tb_ring *ring = f->sdev.rx_ring.ring;
    struct sgu_hw_frame *terminal = queued_frame(ring, 0);
    struct ring_frame *rf = &terminal->frame;

    assert(rf->buffer_phy == ctx->terminal_dma[SGU_DIRECTION_RX][slot]);
    memcpy(page_address(ctx->terminal_page[SGU_DIRECTION_RX][slot]), bytes, size);
    rf->size = size == 4096 ? 0 : size;
    rf->sof = size <= 256 ? 0 : 1; rf->eof = 2;
    list_del_init(&rf->list);
    rf->callback(ring, rf, false);
}

static void test_terminal_header(void)
{
    struct fixture f;
    struct sgu_terminal_header header;

    fixture_open(&f, 2, 64, true);
    struct sgu_queue_config q = { .fence = f.sdev.gpu_context->identity };
    assert(tbstream_gpu_ioctl(&f.file, SGU_IOC_QUEUE, (unsigned long)&q) == 0);
    assert(q.features & SGU_CAP_TERMINAL_HEADER);
    /* Unknown flags, and the header mode with another terminal size. */
    assert(prepare_header(&f, 0, 0, 1, 1, 0, 8192, 64, 2) == -EINVAL);
    assert(prepare_header(&f, 0, 0, 1, 1, 0, 8192, 128, SGU_PREPARE_TERMINAL_HEADER) == -EINVAL);
    assert(!live_pages);

    /* TX: the kernel sends its own header, built from the operation. */
    assert(prepare_header(&f, 0, 1, 1, 7, 0, 8192, 64, SGU_PREPARE_TERMINAL_HEADER) == 0);
    assert(live_pages == 1);
    assert(action(&f, SGU_IOC_SUBMIT, 0, 1, 7) == 0);
    struct tb_ring *tx = f.sdev.tx_ring.ring;
    struct sgu_hw_frame *tx_terminal = queued_frame(tx, 2);
    struct tbstream_gpu_context *ctx = f.sdev.gpu_context;
    assert(tx_terminal->frame.buffer_phy == ctx->terminal_dma[0][1]);
    assert(tx_terminal->frame.size == 64);
    assert(queued_frame(tx, 0)->frame.buffer_phy == queued_frame(tx, 0)->window_phy);
    assert(sgu_terminal_valid(page_address(ctx->terminal_page[0][1]), 64,
                              ctx->identity.connection_epoch, 7, 8192, 2, 1));
    complete_all(tx);
    consume_event(&f, 0, 1, 3);
    assert(action(&f, SGU_IOC_RETIRE, 0, 1, 7) == 0);

    /* RX, matching peer header: complete. */
    assert(prepare_header(&f, 1, 0, 2, 7, 0, 8192, 64, SGU_PREPARE_TERMINAL_HEADER) == 0);
    assert(action(&f, SGU_IOC_SUBMIT, 1, 2, 7) == 0);
    struct tb_ring *rx = f.sdev.rx_ring.ring;
    complete_one(rx); complete_one(rx);
    sgu_terminal_build(&header, ctx->identity.connection_epoch, 7, 8192, 2, 0);
    deliver_rx_terminal(&f, 0, &header, sizeof(header));
    consume_event(&f, 1, 2, 3);
    assert(action(&f, SGU_IOC_RETIRE, 1, 2, 7) == 0);
    assert(!terminal_warnings);

    /* RX, peer one operation ahead (shifted stream): clear error, not data. */
    assert(prepare_header(&f, 1, 0, 3, 8, 0, 8192, 64, SGU_PREPARE_TERMINAL_HEADER) == 0);
    assert(action(&f, SGU_IOC_SUBMIT, 1, 3, 8) == 0);
    complete_one(rx); complete_one(rx);
    sgu_terminal_build(&header, ctx->identity.connection_epoch, 9, 8192, 2, 0);
    deliver_rx_terminal(&f, 0, &header, sizeof(header));
    struct sgu_event e = read_event(&f);
    assert(e.status == SGU_EVENT_FAILED && e.count == 3);
    assert(e.frames[2].error_flags == SGU_FRAME_ERROR_TERMINAL);
    assert(!e.frames[0].error_flags && !e.frames[1].error_flags);
    assert(terminal_warnings == 1 && ctx->failed);
    fixture_close(&f);
    assert(!live_pages && !atomic_load(&module_refs));
}

/* A payload frame landing in the terminal slot (peers disagree on size) and
 * the other header fields, each on its own. */
static void test_terminal_mismatches(void)
{
    struct sgu_terminal_header good, bad;
    unsigned char payload[4096];

    sgu_terminal_build(&good, 5, 11, 4096 * 3 + 64, 4, 2);
    assert(sgu_terminal_valid(&good, 64, 5, 11, 4096 * 3 + 64, 4, 2));
    assert(!sgu_terminal_valid(&good, 4096, 5, 11, 4096 * 3 + 64, 4, 2));
    assert(!sgu_terminal_valid(&good, 63, 5, 11, 4096 * 3 + 64, 4, 2));
    assert(!sgu_terminal_valid(&good, 64, 6, 11, 4096 * 3 + 64, 4, 2));  /* stale epoch */
    assert(!sgu_terminal_valid(&good, 64, 5, 10, 4096 * 3 + 64, 4, 2));  /* order */
    assert(!sgu_terminal_valid(&good, 64, 5, 11, 4096 * 3, 4, 2));       /* size */
    assert(!sgu_terminal_valid(&good, 64, 5, 11, 4096 * 3 + 64, 3, 2));
    assert(!sgu_terminal_valid(&good, 64, 5, 11, 4096 * 3 + 64, 4, 1));  /* slot */
    bad = good; bad.magic ^= 1;
    assert(!sgu_terminal_valid(&bad, 64, 5, 11, 4096 * 3 + 64, 4, 2));
    bad = good; bad.version = sgu_le16(2);
    assert(!sgu_terminal_valid(&bad, 64, 5, 11, 4096 * 3 + 64, 4, 2));
    bad = good; bad.reserved[1] = 1;
    assert(!sgu_terminal_valid(&bad, 64, 5, 11, 4096 * 3 + 64, 4, 2));
    memset(payload, 0x5a, sizeof(payload));
    assert(!sgu_terminal_valid((const void *)payload, 64, 5, 11, 4096 * 3 + 64, 4, 2));
    assert(sizeof(struct sgu_terminal_header) == SGU_TERMINAL_HEADER_BYTES);
    /* Wire format is little-endian. */
    const unsigned char *raw = (const void *)&good;
    assert(raw[0] == 'S' && raw[1] == 'G' && raw[2] == 'U' && raw[3] == 'T');

    /* In a live session: a full payload frame where the terminal belongs. */
    struct fixture f;
    fixture_open(&f, 1, 64, true);
    assert(prepare_header(&f, 1, 0, 1, 1, 0, 4096, 64, SGU_PREPARE_TERMINAL_HEADER) == 0);
    assert(action(&f, SGU_IOC_SUBMIT, 1, 1, 1) == 0);
    complete_one(f.sdev.rx_ring.ring);
    deliver_rx_terminal(&f, 0, payload, sizeof(payload));
    struct sgu_event e = read_event(&f);
    assert(e.status == SGU_EVENT_FAILED);
    assert(e.frames[1].error_flags & SGU_FRAME_ERROR_TERMINAL);
    assert(e.frames[1].error_flags & SGU_FRAME_ERROR_LENGTH);
    fixture_close(&f);
    assert(!live_pages && !atomic_load(&module_refs));
}

/* Legacy RX standby reuse keeps the posted terminal buffer: the mode cannot
 * change on the reposted slots. */
static void test_terminal_standby_reuse(void)
{
    struct fixture f;
    struct sgu_terminal_header header;

    fixture_open(&f, 1, 64, true);
    assert(prepare_header(&f, 1, 0, 1, 1, 0, 4096, 64, SGU_PREPARE_TERMINAL_HEADER) == 0);
    assert(action(&f, SGU_IOC_SUBMIT, 1, 1, 1) == 0);
    complete_one(f.sdev.rx_ring.ring);
    sgu_terminal_build(&header, f.sdev.gpu_context->identity.connection_epoch, 1, 4096, 1, 0);
    deliver_rx_terminal(&f, 0, &header, sizeof(header));
    consume_event(&f, 1, 1, 2);
    assert(action(&f, SGU_IOC_RX_REPOST, 1, 1, 1) == 0);
    assert(action(&f, SGU_IOC_RETIRE, 1, 1, 1) == 0);
    assert(prepare_header(&f, 1, 0, 2, 2, 0, 4096, 64, 0) == -EOPNOTSUPP);
    assert(prepare_header(&f, 1, 0, 2, 2, 0, 4096, 64, SGU_PREPARE_TERMINAL_HEADER) == 0);
    assert(action(&f, SGU_IOC_SUBMIT, 1, 2, 2) == 0);
    complete_one(f.sdev.rx_ring.ring);
    sgu_terminal_build(&header, f.sdev.gpu_context->identity.connection_epoch, 2, 4096, 1, 0);
    deliver_rx_terminal(&f, 0, &header, sizeof(header));
    consume_event(&f, 1, 2, 2);
    fixture_close(&f);
    assert(!live_pages && !atomic_load(&module_refs));
}
