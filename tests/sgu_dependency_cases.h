/* SPDX-License-Identifier: GPL-2.0-only */
/* Invoke the real patched driver, never treat a failed fence as readiness. */
static void dependency_signaled_error(void)
{
    for (unsigned direction = 0; direction < 2; ++direction) {
        struct fixture f;
        fixture_open(&f, 2, 64, true);
        struct dma_fence *dep = foreign_fence(&f.resv[direction]);
        dma_fence_set_error(dep, -EIO);
        dma_fence_signal(dep);
        assert(prepare(&f, direction, 0, 1, 1, 0, 4096) == 0);
        int before = dma_syncs;
        assert(action(&f, SGU_IOC_SUBMIT, direction, 1, 1) == -EIO);
        assert(!f.sdev.gpu_context->operations[direction][0].fence);
        assert(!f.sdev.gpu_context->lifetime.pending_callbacks);
        assert(dma_syncs == before);
        resv_clear(&f.resv[direction]);
        /* Error is sticky even if the exporter drops its completed fence. */
        assert(action(&f, SGU_IOC_SUBMIT, direction, 1, 1) == -EIO);
        dma_fence_put(dep);
        fixture_close(&f);
        assert(!atomic_load(&module_refs));
    }
}

static void dependency_wait_error_pruned(void)
{
    struct fixture f;
    fixture_open(&f, 2, 64, true);
    struct dma_fence *dep = foreign_fence(&f.resv[0]);
    assert(prepare(&f, 0, 0, 1, 1, 0, 4096) == 0);
    struct submit_thread t = { .f = &f, .a = {
        .fence = f.sdev.gpu_context->identity, .handle = 1, .sequence = 1 } };
    pthread_t worker;
    assert(!pthread_create(&worker, NULL, submit_worker, &t));
    wait_for_fence_waiter();
    mutex_lock(&f.resv[0].lock);
    resv_clear(&f.resv[0]);
    mutex_unlock(&f.resv[0].lock);
    dma_fence_set_error(dep, -ECANCELED);
    dma_fence_signal(dep);
    assert(!pthread_join(worker, NULL));
    assert(t.result == -ECANCELED);
    assert(!f.sdev.gpu_context->operations[0][0].fence);
    assert(action(&f, SGU_IOC_SUBMIT, 0, 1, 1) == -ECANCELED);
    dma_fence_put(dep);
    fixture_close(&f);
    assert(!atomic_load(&module_refs));
}

static void dependency_wait_stale(void)
{
    struct fixture f;
    fixture_open(&f, 2, 64, true);
    struct dma_fence *dep = foreign_fence(&f.resv[0]);
    assert(prepare(&f, 0, 0, 1, 1, 0, 4096) == 0);
    struct submit_thread t = { .f = &f, .a = {
        .fence = f.sdev.gpu_context->identity, .handle = 1, .sequence = 1 } };
    pthread_t worker;
    assert(!pthread_create(&worker, NULL, submit_worker, &t));
    wait_for_fence_waiter();
    assert(action(&f, SGU_IOC_STOP, 0, 0, 0) == 0);
    assert(action(&f, SGU_IOC_RELEASE, 0, 0, 0) == 0);
    dma_fence_set_error(dep, -EIO);
    dma_fence_signal(dep);
    assert(!pthread_join(worker, NULL));
    assert(t.result == -ESTALE);
    assert(!f.sdev.gpu_context->operations[0][0].dependency_error);
    dma_fence_put(dep);
    fixture_close(&f);
    assert(!atomic_load(&module_refs));
}

static void dependency_nonblocking_pruned(void)
{
    struct fixture f;
    fixture_open(&f, 2, 64, true);
    struct dma_fence *dep = foreign_fence(&f.resv[0]);
    assert(prepare(&f, 0, 0, 1, 1, 0, 4096) == 0);
    f.file.f_flags = O_NONBLOCK;
    assert(action(&f, SGU_IOC_SUBMIT, 0, 1, 1) == -EAGAIN);
    resv_clear(&f.resv[0]);
    assert(action(&f, SGU_IOC_SUBMIT, 0, 1, 1) == -EAGAIN);
    dma_fence_set_error(dep, -EIO);
    dma_fence_signal(dep);
    assert(action(&f, SGU_IOC_SUBMIT, 0, 1, 1) == -EIO);
    assert(!f.sdev.gpu_context->lifetime.pending_callbacks);
    dma_fence_put(dep);
    fixture_close(&f);
    assert(!atomic_load(&module_refs));
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    if (!strcmp(argv[1], "signaled_error")) dependency_signaled_error();
    else if (!strcmp(argv[1], "wait_error_pruned")) dependency_wait_error_pruned();
    else if (!strcmp(argv[1], "wait_stale")) dependency_wait_stale();
    else if (!strcmp(argv[1], "nonblocking_pruned")) dependency_nonblocking_pruned();
    else return 2;
    printf("PASS %s\n", argv[1]);
    return 0;
}
