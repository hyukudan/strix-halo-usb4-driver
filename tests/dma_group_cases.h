/* SPDX-License-Identifier: GPL-2.0-only */
static void test_group_inventory(void)
{
    struct recovery_fixture f;
    bool performed = false;
    (void)performed;
    recovery_init(&f);
    struct tb_ring *all[] = { &f.tx, &f.rx, &f.other };
    f.nhi.tx_rings[4] = &f.other;
    assert(nhi_dma_reset_group_begin(&f.nhi, all, 2) == -EBUSY);
    assert(!f.nhi.dma_resetting);
    assert(nhi_dma_reset_group_begin(&f.nhi, all, 3) == 0);
    nhi_dma_reset_end(&f.nhi);
    f.other.running = true;
    assert(nhi_dma_reset_group_begin(&f.nhi, all, 3) == -EBUSY);
    f.other.running = false;
    f.other.queue.next = &f.tx.queue;
    assert(nhi_dma_reset_group_begin(&f.nhi, all, 3) == -EBUSY);
    INIT_LIST_HEAD(&f.other.queue);
    all[2] = &f.rx;
    assert(nhi_dma_reset_group_begin(&f.nhi, all, 3) == -EINVAL);
    all[2] = &f.tx0;
    assert(nhi_dma_reset_group_begin(&f.nhi, all, 3) == -EINVAL);
    all[2] = NULL;
    assert(nhi_dma_reset_group_begin(&f.nhi, all, 3) == -EINVAL);
    assert(nhi_dma_reset_group_begin(&f.nhi, NULL, 3) == -EINVAL);
    assert(nhi_dma_reset_group_begin(&f.nhi, all, 33) == -EINVAL);
    all[2] = &f.other;
    f.other.nhi = NULL;
    assert(nhi_dma_reset_group_begin(&f.nhi, all, 3) == -EINVAL);
    f.other.nhi = &f.nhi;
    f.nhi.dma_shutdown = true;
    assert(nhi_dma_reset_group_begin(&f.nhi, all, 3) == -ENODEV);
    assert(!writes && !starts && !stops);
}
static void test_group_reset(void)
{
    struct recovery_fixture f;
    bool performed = false;
    (void)performed;
    recovery_init(&f);
    struct tb_ring *all[] = { &f.tx, &f.rx, &f.other };
    f.nhi.tx_rings[4] = &f.other;
    assert(tb_domain_reset_dma_group(&f.tb, all, 2, 0, &performed) == -EBUSY);
    assert(!writes && !starts && !stops);
    inventory_result = 1;
    assert(tb_domain_reset_dma_group(&f.tb, all, 3, 0, &performed) == -EBUSY);
    assert(!writes && !starts && !stops);
    inventory_result = 0;
    assert(tb_domain_reset_dma_group(&f.tb, all, 3, 0, &performed) == 0);
    assert(performed && writes == 1 && starts == 1 && stops == 1 && !pm_refs);
    assert(!f.nhi.dma_resetting && !f.tx.running && !f.other.running);
    assert(f.tx0.running && f.rx0.running);
}
static void test_core_notifier(void)
{
    struct recovery_fixture f;
    struct notifier_block block = {0};
    recovery_init(&f);
    assert(tb_register_dma_lifecycle_notifier(&block) == 0);
    tb_dma_retire_domain_clients(&f.tb);
    assert(core_notify_calls == 1);
    assert(tb_unregister_dma_lifecycle_notifier(&block) == 0);
    assert(!writes && !starts && !stops);
}

static unsigned consumer_retain_quiescing, consumer_retain_revoked, consumer_calls_after_abandon;
static void scripted_consumer(unsigned long event, struct tb *tb)
{
    if (event == TB_DMA_DOMAIN_QUIESCING)
        for (unsigned i = 0; i < consumer_retain_quiescing; ++i)
            tb_domain_dma_retained(tb);
    else if (event == TB_DMA_DOMAIN_DMA_REVOKED)
        for (unsigned i = 0; i < consumer_retain_revoked; ++i)
            tb_domain_dma_retained(tb);
    else if (event == TB_DMA_DOMAIN_ABANDONED)
        ++consumer_calls_after_abandon;
}
static void teardown_case(struct recovery_fixture *f, unsigned quiescing, unsigned revoked)
{
    recovery_init(f);
    f->pci.master = true;
    f->nhi.dma_shutdown = true;
    core_notify_calls = 0; consumer_calls_after_abandon = 0;
    memset(core_events, 0, sizeof(core_events));
    consumer_retain_quiescing = quiescing; consumer_retain_revoked = revoked;
    teardown_consumer = scripted_consumer;
}
static void test_teardown_revoke(void)
{
    struct recovery_fixture f;
    int before;

    /* Nothing retained: upstream behaviour, bus mastering untouched. */
    teardown_case(&f, 0, 0);
    tb_dma_retire_domain_clients(&f.tb);
    assert(core_notify_calls == 1 && core_events[0] == TB_DMA_DOMAIN_QUIESCING);
    assert(f.pci.master && !f.nhi.dma_revoked && !f.nhi.dma_abandoned);
    /* Retained, then released once bus mastering is revoked and verified. */
    teardown_case(&f, 2, 0);
    before = reads;
    tb_dma_retire_domain_clients(&f.tb);
    assert(core_notify_calls == 2 && core_events[1] == TB_DMA_DOMAIN_DMA_REVOKED);
    assert(!f.pci.master && f.nhi.dma_revoked && !f.nhi.dma_abandoned && reads == before + 1);
    /* Still retained after the revoke: abandoned, never waited on forever. */
    teardown_case(&f, 1, 1);
    tb_dma_retire_domain_clients(&f.tb);
    assert(core_notify_calls == 3 && core_events[2] == TB_DMA_DOMAIN_ABANDONED);
    assert(f.nhi.dma_revoked && f.nhi.dma_abandoned && consumer_calls_after_abandon == 1);
    /* Bus master bit cannot be cleared: no proof, no release. */
    teardown_case(&f, 1, 0);
    f.pci.master_stuck = true;
    tb_dma_retire_domain_clients(&f.tb);
    assert(core_notify_calls == 2 && core_events[1] == TB_DMA_DOMAIN_ABANDONED);
    assert(!f.nhi.dma_revoked && f.nhi.dma_abandoned);
    /* Config space read fails: no proof either. */
    teardown_case(&f, 1, 0);
    f.pci.config_fail = true;
    tb_dma_retire_domain_clients(&f.tb);
    assert(core_events[1] == TB_DMA_DOMAIN_ABANDONED && f.nhi.dma_abandoned);
    /* Not a PCI function: no revoke mechanism. */
    teardown_case(&f, 1, 0);
    f.pci.dev.pci = false;
    tb_dma_retire_domain_clients(&f.tb);
    assert(core_events[1] == TB_DMA_DOMAIN_ABANDONED && f.nhi.dma_abandoned && f.pci.master);
    /* A function that does not answer is no proof (series rule): abandon. */
    teardown_case(&f, 1, 0);
    f.pci.present = false;
    tb_dma_retire_domain_clients(&f.tb);
    assert(core_events[1] == TB_DMA_DOMAIN_ABANDONED && !f.nhi.dma_revoked && f.nhi.dma_abandoned);
    /* Audit A5: the read after the revoke returned all ones (it did not
     * complete at the function): no proof, even with BME cleared. */
    teardown_case(&f, 1, 0);
    caps = ~0U;
    before = reads;
    tb_dma_retire_domain_clients(&f.tb);
    assert(reads == before + 1 && !f.pci.master);
    assert(core_events[1] == TB_DMA_DOMAIN_ABANDONED && !f.nhi.dma_revoked && f.nhi.dma_abandoned);
    /* A function answering with no hop count is not this NHI any more. */
    teardown_case(&f, 1, 0);
    caps = 0x200000;
    before = reads;
    tb_dma_retire_domain_clients(&f.tb);
    assert(reads == before + 1 && !f.pci.master);
    assert(core_events[1] == TB_DMA_DOMAIN_ABANDONED && !f.nhi.dma_revoked && f.nhi.dma_abandoned);
    caps = 0x200008;
    /* Relaxed Ordering lets earlier posted writes pass that read. */
    teardown_case(&f, 1, 0);
    f.pci.relaxed = true;
    tb_dma_retire_domain_clients(&f.tb);
    assert(core_events[1] == TB_DMA_DOMAIN_ABANDONED && !f.nhi.dma_revoked && f.nhi.dma_abandoned);
    /* The ordering premise cannot be read either. */
    teardown_case(&f, 1, 0);
    f.pci.devctl_fail = true;
    tb_dma_retire_domain_clients(&f.tb);
    assert(core_events[1] == TB_DMA_DOMAIN_ABANDONED && f.nhi.dma_abandoned);
    teardown_consumer = NULL;
    assert(!writes && !starts && !stops);
}
