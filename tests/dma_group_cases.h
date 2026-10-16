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
