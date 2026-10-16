/* SPDX-License-Identifier: GPL-2.0-only */
/* Full gpu_stream.c functions with explicit device/CM doubles. */
static void make_detached_quarantine(struct fixture *f)
{
    fixture_open(f, 2, 64, true);
    assert(prepare(f, 1, 0, 1, 1, 0, 4096) == 0);
    assert(action(f, SGU_IOC_SUBMIT, 1, 1, 1) == 0);
    f->xd.is_unplugged = true;
    domain_paths_busy = 1;
    mutex_lock(&f->sdev.lock);
    tbstream_gpu_disconnect(&f->sdev);
    f->sdev.stream = NULL;
    assert(tbstream_gpu_paths_pending(&f->sdev, &f->stream));
    tbstream_gpu_close(&f->sdev);
    mutex_unlock(&f->sdev.lock);
    assert(f->sdev.gpu_context->quarantined);
    assert(f->sdev.gpu_context->lifetime.stop_confirmed);
    assert(!f->sdev.gpu_context->lifetime.paths_removed);
    assert(f->sdev.gpu_context->detached_hopids);
}

static void test_detached_reap(void)
{
    struct fixture f;
    make_detached_quarantine(&f);
    int maps_before = unmaps;
    assert(reap(&f, false) == -EIO && unmaps == maps_before);
    domain_paths_busy = 0; /* CM double now proves actual old tunnel removal. */
    assert(reap(&f, false) == 0);
    assert(!f.sdev.gpu_context && !f.sdev.refs && !workqueues);
    assert(unmaps == maps_before + 2);
    assert(last_path_domain == &f.tb);
    assert(last_hopid_xd == &f.xd && released_in_hopid == 8 && released_out_hopid == 9);
    assert(!f.nhi.dev.locked);
    fixture_close(&f);
    assert(!atomic_load(&module_refs));
}

static void test_detached_path_proof(void)
{
    struct fixture f;
    make_detached_quarantine(&f);
    int maps_before = unmaps;
    for (int i = 0; i < 4; ++i) {
        const int unavailable[] = {1, -EAGAIN, -EOPNOTSUPP, -EIO};
        domain_paths_busy = unavailable[i];
        assert(reap(&f, false) == -EIO && unmaps == maps_before);
        assert(f.sdev.gpu_context && !released_in_hopid && !released_out_hopid);
    }
    domain_paths_busy = 0;
    assert(reap(&f, false) == 0);
    fixture_close(&f);
    assert(!atomic_load(&module_refs));
}

static void test_detached_device_guard(void)
{
    struct fixture f;
    make_detached_quarantine(&f);
    int queries = path_queries, calls = disable_calls, maps_before = unmaps;
    domain_paths_busy = 0;
    device_lock_busy = true;
    assert(reap(&f, false) == -EAGAIN);
    device_lock_busy = false;
    f.nhi.dev.bound = false;
    assert(reap(&f, false) == -ENODEV);
    f.nhi.dev.bound = true;
    f.nhi.dev.driver_data = NULL;
    assert(reap(&f, false) == -ENODEV);
    f.nhi.dev.driver_data = &f.tb;
    f.nhi.going_away = true;
    assert(reap(&f, false) == -ENODEV);
    f.nhi.going_away = false;
    f.nhi.dma_shutdown = true;
    assert(reap(&f, false) == -ENODEV);
    f.nhi.dma_shutdown = false;
    f.sdev.users = 1;
    assert(reap(&f, false) == -EBUSY);
    f.sdev.users = 0;
    assert(path_queries == queries && disable_calls == calls && unmaps == maps_before);
    assert(!f.nhi.dev.locked);
    assert(reap(&f, false) == 0);
    fixture_close(&f);
    assert(!atomic_load(&module_refs));
}

static void test_detached_reset_rejected(void)
{
    struct fixture f;
    struct tb new_tb = {0};
    struct tb_xdomain new_xd = {.tb = &new_tb};
    struct tb_service new_svc = {.xd = &new_xd};
    struct tbstream new_stream = {.svc = &new_svc};
    make_detached_quarantine(&f);
    assert(reap(&f, true) == -ENODEV && !reset_calls);
    f.sdev.stream = &new_stream;
    f.sdev.in_hopid = 88;
    f.sdev.out_hopid = 99;
    mutex_lock(&f.sdev.lock);
    /* A new stream detaching cannot transfer its own HopIDs to the old ctx. */
    assert(!tbstream_gpu_paths_pending(&f.sdev, &new_stream));
    assert(f.sdev.gpu_context->detached_hopids);
    mutex_unlock(&f.sdev.lock);
    /* Force another outstanding disable so immutable old HopIDs are exercised. */
    f.sdev.gpu_context->armed = true;
    domain_paths_busy = 0;
    assert(reap(&f, true) == -ENODEV && !reset_calls);
    assert(reap(&f, false) == 0);
    assert(last_disabled_xd == &f.xd && last_disable_rx == 8 && last_disable_tx == 9);
    assert(last_hopid_xd == &f.xd && released_in_hopid == 8 && released_out_hopid == 9);
    assert(f.sdev.stream == &new_stream && f.sdev.in_hopid == 88 && f.sdev.out_hopid == 99);
    fixture_close(&f);
    assert(!atomic_load(&module_refs));
}
