/* SPDX-License-Identifier: GPL-2.0-only */
/* Domain teardown with a context that cannot prove its DMA stopped, and
 * configfs rmdir of a quarantined context. Included after the group cases. */

/* Frames the controller dropped without a callback: a stop no longer cancels
 * them, so the operation keeps pending callbacks and an unsignaled fence. */
static void lose_queued_frames(struct tb_ring *ring)
{
    while (!list_empty(&ring->queue))
        list_del_init(ring->queue.next);
}

static struct dma_fence *submit_rx(struct fixture *f)
{
    assert(prepare(f, 1, 0, 1, 1, 0, 4096) == 0);
    assert(action(f, SGU_IOC_SUBMIT, 1, 1, 1) == 0);
    return dma_fence_get(f->sdev.gpu_context->operations[1][0].fence);
}

static void test_domain_revoked_releases_retained(void)
{
    struct fixture f;
    fixture_open(&f, 2, 64, true);
    group_file(&f, 80);
    assert(tbstream_gpu_lifecycle_init() == 0);
    struct dma_fence *fence = submit_rx(&f);
    lose_queued_frames(f.sdev.rx_ring.ring);
    f.nhi.dma_shutdown = true;
    retained_reports = 0;
    assert(sgu_domain_quiescing(NULL, TB_DMA_DOMAIN_QUIESCING, &f.tb) == NOTIFY_OK);
    /* Ring/callback proof is missing: retained and reported to the core. */
    assert(f.sdev.gpu_context && f.sdev.gpu_context->quarantined);
    assert(retained_reports == 1 && !dma_fence_is_signaled(fence));
    assert(atomic_load(&f.buffers[1].refs) == 2);
    /* A ring whose software state still runs is not released even then. */
    f.sdev.rx_ring.ring->running = true;
    stop_fails = true;
    assert(sgu_domain_quiescing(NULL, TB_DMA_DOMAIN_DMA_REVOKED, &f.tb) == NOTIFY_OK);
    assert(f.sdev.gpu_context && retained_reports == 2 && !dma_fence_is_signaled(fence));
    stop_fails = false;
    /* Bus mastering revoked: release, and fail what can never complete. */
    assert(sgu_domain_quiescing(NULL, TB_DMA_DOMAIN_DMA_REVOKED, &f.tb) == NOTIFY_OK);
    assert(!f.sdev.gpu_context && retained_reports == 2);
    assert(dma_fence_is_signaled(fence) && fence->error == -ENODEV);
    assert(list_empty(&sgu_clients) && !f.sdev.refs);
    assert(atomic_load(&f.buffers[1].refs) == 1);
    assert(tbstream_gpu_poll(&f.file, NULL) == (EPOLLERR | EPOLLHUP));
    dma_fence_put(fence);
    tbstream_gpu_lifecycle_exit();
    fd_table[80] = NULL; f.sdev.users = 0;
    fixture_close(&f);
    assert(!atomic_load(&module_refs));
}

static void test_domain_abandoned_leaks_safely(void)
{
    struct fixture f;
    fixture_open(&f, 2, 64, true);
    group_file(&f, 80);
    struct dma_fence *fence = submit_rx(&f);
    struct tbstream_gpu_context *ctx = f.sdev.gpu_context;
    lose_queued_frames(f.sdev.rx_ring.ring);
    f.nhi.dma_shutdown = true;
    retained_reports = 0;
    assert(sgu_domain_quiescing(NULL, TB_DMA_DOMAIN_QUIESCING, &f.tb) == NOTIFY_OK);
    assert(retained_reports == 1);
    f.sdev.tx_ring.ring->running = true; /* pretend the controller still runs */
    assert(sgu_domain_quiescing(NULL, TB_DMA_DOMAIN_ABANDONED, &f.tb) == NOTIFY_OK);
    assert(ctx->abandoned && ctx->quarantined && retained_reports == 1);
    /* Nothing reaches the controller or frees DMA memory any more. */
    u64 stops = ctx->stats.dma_stops;
    int before = unmaps;
    struct sgu_query query = {0};
    assert(tbstream_gpu_ioctl(&f.file, SGU_IOC_QUERY, (unsigned long)&query) == -ENODEV);
    mutex_lock(&f.sdev.lock);
    tbstream_gpu_stop(&f.sdev);
    tbstream_gpu_disconnect(&f.sdev);
    tbstream_gpu_close(&f.sdev);
    mutex_unlock(&f.sdev.lock);
    f.sdev.users = 0;
    assert(reap(&f, false) == -ENODEV);
    assert(f.sdev.gpu_context == ctx && ctx->stats.dma_stops == stops && unmaps == before);
    assert(f.sdev.tx_ring.ring->running && !dma_fence_is_signaled(fence));
    assert(sgu_domain_quiescing(NULL, TB_DMA_DOMAIN_DMA_REVOKED, &f.tb) == NOTIFY_OK);
    assert(f.sdev.gpu_context == ctx);
    assert(tbstream_gpu_poll(&f.file, NULL) & EPOLLERR);
    /* Test-only teardown of what a real abandonment leaks until reboot. */
    f.sdev.tx_ring.ring->running = false;
    ctx->abandoned = false;
    ctx->dma_revoked = true;
    ctx->lifetime.pending_callbacks = 0;
    dma_fence_set_error(fence, -ENODEV);
    dma_fence_signal(fence);
    dma_fence_put(fence);
    fd_table[80] = NULL;
    fixture_close(&f);
    assert(list_empty(&sgu_clients) && !f.sdev.refs && !atomic_load(&module_refs));
}

static void test_orphan_reaping_after_rmdir(void)
{
    struct fixture f;
    fixture_open(&f, 2, 64, true);
    assert(prepare(&f, 1, 0, 1, 1, 0, 4096) == 0);
    assert(action(&f, SGU_IOC_SUBMIT, 1, 1, 1) == 0);
    stop_fails = true;
    disable_fail = 1;
    mutex_lock(&f.sdev.lock);
    tbstream_gpu_close(&f.sdev);
    mutex_unlock(&f.sdev.lock);
    assert(f.sdev.gpu_context && f.sdev.gpu_context->quarantined);
    f.sdev.removed = true; /* configfs rmdir: gpu_recover no longer exists */
    assert(reap(&f, false) == -ENODEV && reap(&f, true) == -ENODEV);
    assert(sgu_reap_orphans_set("x", NULL) == -EINVAL);
    assert(sgu_reap_orphans_set("0", NULL) == 0 && f.sdev.gpu_context);
    admin = false;
    assert(sgu_reap_orphans_set("1", NULL) == -EPERM);
    admin = true;
    assert(sgu_reap_orphans_set("1", NULL) == -EBUSY && f.sdev.gpu_context); /* still unproved */
    f.sdev.users = 1;
    stop_fails = false;
    disable_fail = 0;
    assert(sgu_reap_orphans_set("1", NULL) == -EBUSY && f.sdev.gpu_context); /* FD still open */
    f.sdev.users = 0;
    assert(sgu_reap_orphans_set("1", NULL) == 0);
    assert(!f.sdev.gpu_context && !f.sdev.refs && list_empty(&sgu_clients));
    assert(atomic_load(&module_refs) == 1); /* reservation still owns the fence */
    fixture_close(&f);
    assert(!atomic_load(&module_refs));
}
