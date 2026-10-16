/* SPDX-License-Identifier: GPL-2.0-only */
/* Included after the existing fixture helpers, before the test dispatcher. */
static void *watchdog_worker(void *p)
{
    struct tbstream_gpu_context *ctx = p;
    run_delayed_work(&ctx->watchdog);
    assert(!signalling_depth);
    return NULL;
}

static void join_bounded(pthread_t thread)
{
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += 2;
    assert(pthread_timedjoin_np(thread, NULL, &until) == 0);
}

static void test_watchdog_mutex_independence(void)
{
    struct fixture f;
    fixture_open(&f, 2, 64, true);
    struct tbstream_gpu_context *ctx = f.sdev.gpu_context;
    assert(prepare(&f, 1, 0, 1, 1, 0, 8192) == 0);
    assert(action(&f, SGU_IOC_SUBMIT, 1, 1, 1) == 0);
    struct dma_fence *fence = dma_fence_get(ctx->operations[1][0].fence);
    ctx->operations[1][0].deadline_ns = 0;
    /* Simulate a faulting ioctl and an exporter holding its reservation. */
    mutex_lock(&f.sdev.lock);
    mutex_lock(&f.resv[1].lock);
    alloc_fail_after = 0;
    pthread_t thread;
    assert(!pthread_create(&thread, NULL, watchdog_worker, ctx));
    join_bounded(thread);
    assert(dma_fence_is_signaled(fence) && fence->error == -ETIMEDOUT);
    assert(ctx->lifetime.stop_confirmed && !ctx->lifetime.pending_callbacks);
    assert(ctx->armed && !ctx->lifetime.paths_removed); /* control-plane deferred */
    assert(ctx->stats.timeouts == 1 && ctx->stats.errors[1] == 1);
    assert(alloc_fail_after == 0); /* completion required no allocation */
    alloc_fail_after = -1;
    mutex_unlock(&f.resv[1].lock);
    mutex_unlock(&f.sdev.lock);
    assert(action(&f, SGU_IOC_STOP, 0, 0, 0) == 0);
    assert(sgu_can_release(ctx));
    dma_fence_put(fence);
    fixture_close(&f);
    assert(!workqueues && !atomic_load(&module_refs));
}

static atomic_bool in_signal, allow_signal;
static void stalled_signal(struct dma_fence *fence)
{
    (void)fence;
    atomic_store(&in_signal, true);
    while (!atomic_load(&allow_signal))
        usleep(100);
}

static void *complete_worker(void *ring)
{
    complete_all(ring);
    assert(!signalling_depth);
    return NULL;
}

static void test_fence_precedes_event(void)
{
    struct fixture f;
    fixture_open(&f, 2, 64, true);
    assert(prepare(&f, 0, 0, 1, 1, 0, 8192) == 0);
    assert(action(&f, SGU_IOC_SUBMIT, 0, 1, 1) == 0);
    struct tbstream_gpu_context *ctx = f.sdev.gpu_context;
    struct dma_fence *fence = ctx->operations[0][0].fence;
    atomic_store(&in_signal, false);
    atomic_store(&allow_signal, false);
    signal_hook = stalled_signal;
    pthread_t thread;
    assert(!pthread_create(&thread, NULL, complete_worker, f.sdev.tx_ring.ring));
    unsigned long until = jiffies + 1000;
    while (!atomic_load(&in_signal) && !time_after_eq(jiffies, until))
        usleep(100);
    assert(atomic_load(&in_signal));
    assert(!dma_fence_is_signaled(fence));
    assert(ctx->operations[0][0].phase == SGU_OP_FINISHING);
    struct sgu_event event = { .fence = ctx->identity };
    assert(tbstream_gpu_ioctl(&f.file, SGU_IOC_EVENT, (unsigned long)&event) == -EAGAIN);
    assert(!(tbstream_gpu_poll(&f.file, NULL) & EPOLLIN));
    assert(action(&f, SGU_IOC_RETIRE, 0, 1, 1) == -EBUSY);
    assert(prepare(&f, 0, 0, 2, 2, 0, 8192) == -EBUSY);
    /* Export while signalling is in flight is allowed, but still unsignalled. */
    struct sgu_sync_file request = { .fence = ctx->identity, .handle = 1,
                                    .sequence = 1, .fd = -1 };
    assert(tbstream_gpu_ioctl(&f.file, SGU_IOC_EXPORT_SYNC_FILE, (unsigned long)&request) == 0);
    assert(!dma_fence_is_signaled(fd_table[request.fd]->sync->fence));
    atomic_store(&allow_signal, true);
    join_bounded(thread);
    signal_hook = NULL;
    assert(dma_fence_is_signaled(fence));
    consume_event(&f, 0, 1, 3);
    assert(action(&f, SGU_IOC_RETIRE, 0, 1, 1) == 0);
    fput(fd_table[request.fd]);
    fd_table[request.fd] = NULL;
    fixture_close(&f);
    assert(!workqueues && !atomic_load(&module_refs));
}

static struct tbstream_gpu_context *stop_during_allocation;
static void allocation_stops_dma(void)
{
    if (!stop_during_allocation)
        return;
    struct tbstream_gpu_context *ctx = stop_during_allocation;
    stop_during_allocation = NULL;
    pthread_t thread;
    assert(!pthread_create(&thread, NULL, watchdog_worker, ctx));
    join_bounded(thread);
}

static void test_stop_before_publication(void)
{
    struct fixture f;
    fixture_open(&f, 2, 64, true);
    struct tbstream_gpu_context *ctx = f.sdev.gpu_context;
    assert(prepare(&f, 1, 0, 1, 1, 0, 4096) == 0);
    assert(action(&f, SGU_IOC_SUBMIT, 1, 1, 1) == 0);
    assert(prepare(&f, 0, 0, 2, 1, 0, 4096) == 0);
    ctx->operations[1][0].deadline_ns = 0;
    stop_during_allocation = ctx;
    allocation_hook = allocation_stops_dma;
    /* Fence allocation is allowed to reclaim/block: watchdog must still run.
     * Revalidation under dma_lock must reject the now-stopped TX submission. */
    assert(action(&f, SGU_IOC_SUBMIT, 0, 2, 1) == -ESHUTDOWN);
    allocation_hook = NULL;
    assert(ctx->operations[0][0].fence == NULL);
    assert(f.resv[0].count == 0 && list_empty(&f.sdev.tx_ring.ring->queue));
    assert(!ctx->lifetime.pending_callbacks && ctx->stats.submissions[0] == 0);
    fixture_close(&f);
    assert(!workqueues && !atomic_load(&module_refs));
}

static int reap(struct fixture *f, bool reset)
{
    mutex_lock(&f->sdev.lock);
    int result = tbstream_gpu_reap(&f->sdev, reset);
    mutex_unlock(&f->sdev.lock);
    return result;
}

static void test_quarantine_reaping(void)
{
    struct fixture f;
    fixture_open(&f, 2, 64, true);
    assert(prepare(&f, 1, 0, 1, 1, 0, 4096) == 0);
    assert(action(&f, SGU_IOC_SUBMIT, 1, 1, 1) == 0);
    struct tbstream_gpu_context *ctx = f.sdev.gpu_context;
    stop_fails = true;
    disable_fail = 1;
    mutex_lock(&f.sdev.lock);
    tbstream_gpu_close(&f.sdev);
    mutex_unlock(&f.sdev.lock);
    assert(ctx->quarantined && ctx->closing && f.sdev.refs == 1);
    int before = unmaps;
    assert(atomic_load(&module_refs) == 2); /* fence + quarantine */
    f.sdev.users = 1;
    assert(reap(&f, false) == -EBUSY);
    f.sdev.users = 0;
    admin = false;
    assert(reap(&f, false) == -EPERM);
    admin = true;
    assert(reap(&f, false) == -EIO && unmaps == before);
    f.sdev.removed = true;
    assert(reap(&f, false) == -ENODEV && unmaps == before);
    f.sdev.removed = false;
    stop_fails = false;
    disable_fail = 1;
    assert(reap(&f, false) == -EIO && unmaps == before);
    disable_fail = 0;
    reset_error = -EBUSY;
    assert(reap(&f, true) == -EBUSY && unmaps == before);
    reset_error = 0;
    assert(reap(&f, true) == 0);
    assert(!f.sdev.gpu_context && !f.sdev.refs && !workqueues);
    assert(atomic_load(&module_refs) == 1); /* last reservation still owns fence */
    fixture_close(&f);
    assert(!atomic_load(&module_refs));
}

static void test_explicit_controller_reset(void)
{
    struct fixture f;
    fixture_open(&f, 2, 64, true);
    struct tbstream_gpu_context *ctx = f.sdev.gpu_context;
    assert(action(&f, SGU_IOC_RESET, 0, 0, 0) == -EBUSY);
    assert(reset_calls == 0);
    assert(action(&f, SGU_IOC_STOP, 0, 0, 0) == 0);
    admin = false;
    assert(action(&f, SGU_IOC_RESET, 0, 0, 0) == -EPERM && reset_calls == 0);
    admin = true;
    struct sgu_fence old = ctx->identity;
    reset_error = -EBUSY;
    assert(action(&f, SGU_IOC_RESET, 0, 0, 0) == -EBUSY && ctx->imported);
    assert(!memcmp(&old, &ctx->identity, sizeof(old)));
    reset_error = 0;
    struct sgu_action a = { .fence = ctx->identity };
    assert(tbstream_gpu_ioctl(&f.file, SGU_IOC_RESET, (unsigned long)&a) == 0);
    assert(a.result == SGU_RESULT_CONTROLLER_RESET && !a.fence.import_generation);
    assert(!ctx->imported && ctx->requires_new_epoch);
    assert(ctx->stats.resets == 1 && ctx->stats.reset_failures == 1);
    struct sgu_import req = import_request(&f, 1);
    assert(tbstream_gpu_ioctl(&f.file, SGU_IOC_IMPORT, (unsigned long)&req) == -EINVAL);
    req = import_request(&f, 2);
    assert(tbstream_gpu_ioctl(&f.file, SGU_IOC_IMPORT, (unsigned long)&req) == 0);
    assert(action(&f, SGU_IOC_ARM, 0, 0, 0) == 0);
    assert(prepare(&f, 0, 0, 1, 1, 0, 4096) == 0);
    assert(action(&f, SGU_IOC_SUBMIT, 0, 1, 1) == 0);
    complete_all(f.sdev.tx_ring.ring);
    fixture_close(&f);
    assert(!workqueues && !atomic_load(&module_refs));
}

static void test_statistics_snapshot(void)
{
    struct fixture f;
    fixture_open(&f, 2, 64, true);
    struct tbstream_gpu_context *ctx = f.sdev.gpu_context;
    struct sgu_stats stats = { .fence = ctx->identity };
    assert(tbstream_gpu_ioctl(&f.file, SGU_IOC_STATS, (unsigned long)&stats) == 0);
    assert(stats.queue_depth == 2 && !stats.submissions[0] && !stats.pending_callbacks);
    assert(prepare(&f, 0, 0, 1, 1, 0, 8191) == 0);
    assert(action(&f, SGU_IOC_SUBMIT, 0, 1, 1) == 0);
    assert(prepare(&f, 1, 0, 2, 1, 0, 4096) == 0);
    assert(action(&f, SGU_IOC_SUBMIT, 1, 2, 1) == 0);
    memset(&stats, 0, sizeof(stats)); stats.fence = ctx->identity;
    assert(tbstream_gpu_ioctl(&f.file, SGU_IOC_STATS, (unsigned long)&stats) == 0);
    assert(stats.pending_callbacks == 5 && stats.peak_callbacks == 5);
    complete_all(f.sdev.tx_ring.ring);
    ctx->operations[1][0].deadline_ns = 0;
    run_delayed_work(&ctx->watchdog);
    memset(&stats, 0, sizeof(stats)); stats.fence = ctx->identity;
    assert(tbstream_gpu_ioctl(&f.file, SGU_IOC_STATS, (unsigned long)&stats) == 0);
    assert(stats.submissions[0] == 1 && stats.submissions[1] == 1);
    assert(stats.completions[0] == 1 && stats.completions[1] == 1);
    assert(stats.payload_bytes[0] == 8191 && !stats.payload_bytes[1]);
    assert(!stats.errors[0] && stats.errors[1] == 1 && stats.timeouts == 1);
    assert(!stats.pending_callbacks && stats.dma_stops == 1);
    assert(stats.max_dma_ns[0] >= stats.last_dma_ns[0] && stats.monotonic_ns);
    assert(tbstream_gpu_ioctl(&f.file, SGU_IOC_STATS, (unsigned long)&stats) == -EINVAL);
    memset(&stats, 0, sizeof(stats)); stats.fence = ctx->identity; stats.reserved[0] = 1;
    assert(tbstream_gpu_ioctl(&f.file, SGU_IOC_STATS, (unsigned long)&stats) == -EINVAL);
    fixture_close(&f);
    assert(!workqueues && !atomic_load(&module_refs));
}
