/* SPDX-License-Identifier: GPL-2.0-only */
/* Included after the actual tb_stop() and dma_path_query.h. */
static struct tb_tunnel *tunnel_new(struct tb *tb, bool dma, int tx, int rx)
{
    struct tb_tunnel *t = calloc(1, sizeof(*t));

    assert(t);
    t->dma = dma; t->active = true; t->tx_ring = tx; t->rx_ring = rx;
    list_add_tail(&t->list, &tb->cm.tunnel_list);
    return t;
}

static void fixture(struct tb *tb, struct tb_nhi *nhi, struct tb_switch *root)
{
    memset(tb, 0, sizeof(*tb));
    memset(root, 0, sizeof(*root));
    tb->lock = 1; tb->nhi = nhi; tb->root_switch = root;
    INIT_LIST_HEAD(&tb->cm.tunnel_list);
    tb->cm.hotplug_active = true;
}

/* Domain removal with an armed gpu_direct session: the DMA tunnel is still in
 * the CM inventory when tb_stop() runs, and domain retirement queries it
 * afterwards (tb_domain_remove -> retire -> tbstream_gpu_stop). */
static void stop_then_query(void)
{
    struct tb_nhi nhi = { .host_reset = false };
    struct tb_switch root;
    struct tb tb;

    fixture(&tb, &nhi, &root);
    tunnel_new(&tb, false, 0, 0);       /* PCIe/USB3 tunnel: left in hardware */
    tunnel_new(&tb, true, 2, 3);        /* gpu_direct DMA tunnel */
    tunnel_new(&tb, true, 1, 1);        /* host stream DMA tunnel */
    assert(tb_dma_paths_busy(&tb, 2, 3) == 1);
    tb_stop(&tb);
    assert(deactivated == 2 && puts_done == 3);
    assert(!tb.root_switch && root.removed && !tb.cm.hotplug_active);
    /* Every tunnel was freed: the inventory must be empty, not stale. */
    assert(list_empty(&tb.cm.tunnel_list));
    assert(tb_dma_paths_busy(&tb, 2, 3) == 0);
    assert(tb_dma_paths_busy(&tb, -1, -1) == 0);
}

/* Belt: even if some path left a stale entry after stop, the query must not
 * dereference it and must not answer "no DMA path". */
static void stale_after_stop(void)
{
    struct tb_nhi nhi = { 0 };
    struct tb_switch root;
    struct list_head poisoned = { (void *)0x100, (void *)0x122 };
    struct tb tb;

    fixture(&tb, &nhi, &root);
    tb.root_switch = NULL;
    assert(tb_dma_paths_busy(&tb, 2, 3) == 0);   /* empty: correct proof */
    tb.cm.tunnel_list.next = &poisoned;          /* never walked */
    assert(tb_dma_paths_busy(&tb, 2, 3) == -ENODEV);
    assert(tb_dma_paths_busy(&tb, -1, -1) == -ENODEV);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    if (!strcmp(argv[1], "stop_then_query"))
        stop_then_query();
    else if (!strcmp(argv[1], "stale_after_stop"))
        stale_after_stop();
    else
        return 2;
    printf("PASS %s\n", argv[1]);
    return 0;
}
