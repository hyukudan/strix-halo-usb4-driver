/* SPDX-License-Identifier: GPL-2.0-only */
static void group_file(struct fixture *f, int fd)
{
    f->file.f_op = &tbstream_dev_fops;
    f->sdev.gpu_direct = true;
    f->sdev.users = 1;
    fd_table[fd] = &f->file;
}
static struct sgu_reset_group group_request(struct fixture *a, struct fixture *b)
{
    struct sgu_reset_group group = { .count = 2 };
    group.members[0].fd = 80;
    group.members[0].fence = a->sdev.gpu_context->identity;
    group.members[1].fd = 81;
    group.members[1].fence = b->sdev.gpu_context->identity;
    return group;
}
static void group_open(struct fixture *a, struct fixture *b)
{
    fixture_open(a, 2, 64, true);
    fixture_open(b, 2, 64, true);
    /* Fixture-only rebind: this pair has a single core domain. Core's actual
     * complete-ring inventory is tested separately against its source. */
    b->xd.tb = &a->tb;
    b->sdev.gpu_context->domain_owner = &a->tb.dev;
    b->sdev.tx_ring.ring->nhi = b->sdev.rx_ring.ring->nhi = &a->nhi;
    group_file(a, 80);
    group_file(b, 81);
    assert(action(a, SGU_IOC_STOP, 0, 0, 0) == 0);
    assert(action(b, SGU_IOC_STOP, 0, 0, 0) == 0);
}
static void group_close(struct fixture *a, struct fixture *b)
{
    assert(!a->file.borrowed && !b->file.borrowed);
    fd_table[80] = fd_table[81] = NULL;
    a->sdev.users = b->sdev.users = 0;
    fixture_close(a);
    fixture_close(b);
    assert(list_empty(&sgu_clients));
    assert(!atomic_load(&module_refs));
}
static long group_ioctl(struct fixture *a, struct sgu_reset_group *group)
{
    return tbstream_gpu_ioctl(&a->file, SGU_IOC_RESET_GROUP, (unsigned long)group);
}
static void test_group_success(void)
{
    struct fixture a, b;
    group_open(&a, &b);
    struct sgu_reset_group group = group_request(&a, &b);
    assert(group_ioctl(&a, &group) == 0);
    assert(group.result == SGU_RESULT_GROUP_RESET);
    assert(group_reset_calls == 1 && group_reset_count == 4);
    assert(a.sdev.gpu_context->failed && b.sdev.gpu_context->failed);
    assert(a.sdev.gpu_context->requires_new_epoch && b.sdev.gpu_context->requires_new_epoch);
    assert(action(&a, SGU_IOC_ARM, 0, 0, 0) == -ESHUTDOWN);
    assert(action(&a, SGU_IOC_RECOVER, 0, 0, 0) == 0);
    assert(action(&b, SGU_IOC_RECOVER, 0, 0, 0) == 0);
    struct sgu_import request = import_request(&a, 1);
    assert(tbstream_gpu_ioctl(&a.file, SGU_IOC_IMPORT, (unsigned long)&request) == -EINVAL);
    request = import_request(&a, 2);
    /* Restore fixture FD mapping, which normally is process-local per host. */
    dma_buffers[1] = &a.buffers[0]; dma_buffers[2] = &a.buffers[1];
    assert(tbstream_gpu_ioctl(&a.file, SGU_IOC_IMPORT, (unsigned long)&request) == 0);
    group_close(&a, &b);
}
static void test_group_rejections(void)
{
    struct fixture a, b;
    group_open(&a, &b);
    struct sgu_reset_group original = group_request(&a, &b), group;
    admin = false;
    assert(group_ioctl(&a, &original) == -EPERM);
    admin = true;
    group = original; group.count = 0;
    assert(group_ioctl(&a, &group) == -EINVAL);
    group = original; group.count = SGU_MAX_RESET_MEMBERS + 1;
    assert(group_ioctl(&a, &group) == -EINVAL);
    group = original; group.reserved = 1;
    assert(group_ioctl(&a, &group) == -EINVAL);
    group = original; group.members[2].fd = 1;
    assert(group_ioctl(&a, &group) == -EINVAL);
    group = original; group.members[1].fd = 80;
    assert(group_ioctl(&a, &group) == -EINVAL);
    group = original; group.members[1].fd = 200;
    assert(group_ioctl(&a, &group) == -EBADF);
    group = original; group.members[1].fence.connection_epoch++;
    assert(group_ioctl(&a, &group) == -ESTALE);
    b.sdev.gpu_direct = false;
    assert(group_ioctl(&a, &original) == -ENODEV);
    b.sdev.gpu_direct = true;
    b.xd.tb = &b.tb;
    assert(group_ioctl(&a, &original) == -EXDEV);
    b.xd.tb = &a.tb;
    b.sdev.gpu_context->stopped = false;
    assert(group_ioctl(&a, &original) == -EBUSY);
    b.sdev.gpu_context->stopped = true;
    mutex_lock(&b.sdev.lock);
    assert(group_ioctl(&a, &original) == -EAGAIN);
    mutex_unlock(&b.sdev.lock);
    mutex_lock(&b.sdev.gpu_context->dma_lock);
    assert(group_ioctl(&a, &original) == -EAGAIN);
    mutex_unlock(&b.sdev.gpu_context->dma_lock);
    assert(!group_reset_calls && !a.sdev.gpu_context->failed && !b.sdev.gpu_context->failed);
    assert(mutex_trylock(&a.sdev.lock)); mutex_unlock(&a.sdev.lock);
    assert(mutex_trylock(&a.sdev.gpu_context->dma_lock)); mutex_unlock(&a.sdev.gpu_context->dma_lock);
    group_close(&a, &b);
}
static void test_group_copy_failure(void)
{
    struct fixture a, b;
    group_open(&a, &b);
    struct sgu_reset_group group = group_request(&a, &b);
    copy_from_fail = 1;
    assert(group_ioctl(&a, &group) == -EFAULT && !group_reset_calls);
    reset_error = -EBUSY;
    assert(group_ioctl(&a, &group) == -EBUSY);
    assert(!a.sdev.gpu_context->failed && !b.sdev.gpu_context->failed);
    reset_error = 0; copy_to_fail = 1;
    assert(group_ioctl(&a, &group) == -EFAULT);
    assert(group_reset_calls == 2);
    assert(a.sdev.gpu_context->failed && b.sdev.gpu_context->failed);
    assert(group.result == 0); /* User saw no fabricated successful reply. */
    group_close(&a, &b);
}
static void test_domain_retirement_open_fds(void)
{
    struct fixture a, b;
    group_open(&a, &b);
    assert(tbstream_gpu_lifecycle_init() == 0);
    assert(test_notifier == &sgu_lifecycle_notifier);
    assert(sgu_domain_quiescing(NULL, 999, &a.tb) == NOTIFY_DONE);
    assert(sgu_domain_quiescing(NULL, TB_DMA_DOMAIN_QUIESCING, &a.tb) == NOTIFY_BAD);
    a.nhi.dma_shutdown = true;
    assert(sgu_domain_quiescing(NULL, TB_DMA_DOMAIN_QUIESCING, &a.tb) == NOTIFY_OK);
    assert(!a.sdev.gpu_context && !b.sdev.gpu_context);
    assert(!a.sdev.tx_ring.ring && !b.sdev.rx_ring.ring);
    assert(a.sdev.users == 1 && b.sdev.users == 1); /* FDs were not closed/reused. */
    assert(list_empty(&sgu_clients) && !a.sdev.refs && !b.sdev.refs);
    struct sgu_query query = {0};
    assert(tbstream_gpu_ioctl(&a.file, SGU_IOC_QUERY, (unsigned long)&query) == -ENXIO);
    assert(tbstream_gpu_poll(&a.file, NULL) == (EPOLLERR | EPOLLHUP));
    assert(sgu_domain_quiescing(NULL, TB_DMA_DOMAIN_QUIESCING, &a.tb) == NOTIFY_OK);
    tbstream_gpu_lifecycle_exit();
    assert(!test_notifier);
    group_close(&a, &b);
}
static void test_domain_retirement_keeps_unproved_dma(void)
{
    struct fixture a, b;
    group_open(&a, &b);
    a.nhi.dma_shutdown = true;
    /* A live ring that the injected stop cannot quiesce. A notifier call and
     * an empty path inventory are NOT substitutes for actual DMA stop. */
    a.sdev.tx_ring.ring->running = true;
    stop_fails = true;
    assert(sgu_domain_quiescing(NULL, TB_DMA_DOMAIN_QUIESCING, &a.tb) == NOTIFY_OK);
    assert(a.sdev.gpu_context && a.sdev.gpu_context->quarantined);
    assert(!b.sdev.gpu_context);
    assert(a.sdev.tx_ring.ring->running);
    assert(atomic_load(&a.buffers[0].refs) == 2);
    stop_fails = false;
    assert(sgu_domain_quiescing(NULL, TB_DMA_DOMAIN_QUIESCING, &a.tb) == NOTIFY_OK);
    assert(!a.sdev.gpu_context && list_empty(&sgu_clients));
    group_close(&a, &b);
}
static void test_domain_retirement_foreign_domain(void)
{
    struct fixture a, b;
    group_open(&a, &b);
    b.xd.tb = &b.tb;
    b.sdev.gpu_context->domain_owner = &b.tb.dev;
    a.nhi.dma_shutdown = true;
    assert(sgu_domain_quiescing(NULL, TB_DMA_DOMAIN_QUIESCING, &a.tb) == NOTIFY_OK);
    assert(!a.sdev.gpu_context && b.sdev.gpu_context);
    group_close(&a, &b);
}
static atomic_bool poll_done;
static atomic_uint poll_reads;
static void *retirement_poll_worker(void *data)
{
    struct fixture *f = data;
    while (!atomic_load(&poll_done)) {
        tbstream_gpu_poll(&f->file, NULL);
        atomic_fetch_add(&poll_reads, 1);
    }
    return NULL;
}
static void test_domain_retirement_concurrent_poll(void)
{
    struct fixture a;
    fixture_open(&a, 2, 64, true);
    group_file(&a, 80);
    pthread_t worker;
    assert(!pthread_create(&worker, NULL, retirement_poll_worker, &a));
    while (atomic_load(&poll_reads) < 100) usleep(100);
    a.nhi.dma_shutdown = true;
    assert(sgu_domain_quiescing(NULL, TB_DMA_DOMAIN_QUIESCING, &a.tb) == NOTIFY_OK);
    atomic_store(&poll_done, true);
    assert(!pthread_join(worker, NULL));
    assert(!a.sdev.gpu_context && list_empty(&sgu_clients));
    assert(tbstream_gpu_poll(&a.file, NULL) == (EPOLLERR | EPOLLHUP));
    fd_table[80] = NULL; a.sdev.users = 0;
    fixture_close(&a);
}
