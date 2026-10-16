/* SPDX-License-Identifier: GPL-2.0-only */
static void test_inventory_veto(void)
{
    struct recovery_fixture f;
    recovery_init(&f);
    inventory_result = 1;
    assert(tb_domain_reset_dma(&f.tb, &f.tx, &f.rx) == -EBUSY);
    assert(!writes && !starts && !stops && !pm_refs && !f.nhi.dma_resetting);
    inventory_result = -EIO;
    assert(tb_domain_reset_dma(&f.tb, &f.tx, &f.rx) == -EIO);
    assert(!writes && !starts && !stops && !pm_refs);
    f.tb.cm_ops = NULL;
    assert(tb_domain_reset_dma(&f.tb, &f.tx, &f.rx) == -EOPNOTSUPP);
    assert(!writes && !starts && !stops && !pm_refs);
    f.tb.cm_ops = &fake_cm_ops;
    inventory_result = 0;
    assert(tb_domain_reset_dma(&f.tb, &f.tx, &f.rx) == 0);
    assert(writes == 1);
}
static void test_inventory_query(void)
{
    struct recovery_fixture f;
    recovery_init(&f);
    assert(tb_domain_dma_paths_busy(&f.tb, 0, 2) == -EINVAL);
    assert(tb_domain_dma_paths_busy(&f.tb, -2, 2) == -EINVAL);
    assert(!inventory_calls);
    mutex_lock(&f.tb.lock);
    assert(tb_domain_dma_paths_busy(&f.tb, 2, 3) == -EAGAIN);
    mutex_unlock(&f.tb.lock);
    assert(!inventory_calls);
    f.tb.cm_ops = NULL;
    assert(tb_domain_dma_paths_busy(&f.tb, 2, 3) == -EOPNOTSUPP);
    f.tb.cm_ops = &fake_cm_ops;
    inventory_result = 1;
    assert(tb_domain_dma_paths_busy(&f.tb, 2, 3) == 1);
    inventory_result = 0;
    assert(tb_domain_dma_paths_busy(&f.tb, -1, -1) == 0);
    assert(!writes && !starts && !stops && !pm_refs);
}
