/* SPDX-License-Identifier: GPL-2.0-only */
/* Runs the actual nhi_shutdown() of nhi.c. Rings left allocated at NHI
 * removal (abandoned DMA owners keep them on purpose) must lose every CPU
 * path into them before the NHI, its MMIO and its workqueue are freed: the
 * MSI-X handler (ring_msix dereferences ring->nhi first) and the completion
 * work. Their DMA memory is kept. */
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static char events[32][64];
static int nevents;
#define log_event(...) do { assert(nevents < 32); snprintf(events[nevents++], 64, __VA_ARGS__); } while (0)
static int find(const char *e)
{
    for (int i = 0; i < nevents; ++i)
        if (!strcmp(events[i], e))
            return i;
    return -1;
}

struct device { int unused; };
typedef struct { bool held; } spinlock_t;
static void spin_lock_irq(spinlock_t *l) { assert(!l->held); l->held = true; }
static void spin_unlock_irq(spinlock_t *l) { assert(l->held); l->held = false; }
struct work_struct { bool pending; int id; };
static void cancel_work_sync(struct work_struct *w) { log_event("cancel_work %d", w->id); w->pending = false; }

struct tb_nhi;
struct tb_ring { struct tb_nhi *nhi; int hop, irq; bool is_tx; struct work_struct work; bool freed; };
struct tb_nhi_ops {
    void (*release_ring_irq)(struct tb_ring *ring);
    void (*shutdown)(struct tb_nhi *nhi);
};
struct tb_nhi {
    spinlock_t lock;
    struct device *dev;
    const struct tb_nhi_ops *ops;
    int hop_count;
    struct tb_ring *tx_rings[4], *rx_rings[4];
    bool dma_abandoned;
};

static int errors, warns;
#define dev_dbg(d, ...) ((void)(d))
#define dev_err(d, ...) ((void)(d), ++errors)
#define dev_WARN(d, ...) ((void)(d), ++warns)
static void nhi_disable_interrupts(struct tb_nhi *nhi) { (void)nhi; log_event("mask"); }
static void release_ring_irq(struct tb_ring *ring)
{
    /* free_irq(): waits for a running handler; no handler runs after it */
    if (ring->irq <= 0)
        return;
    log_event("free_irq %d", ring->irq);
    ring->irq = 0;
}
static void shutdown_op(struct tb_nhi *nhi) { (void)nhi; log_event("release_nhi_irq"); }
static const struct tb_nhi_ops ops = { .release_ring_irq = release_ring_irq, .shutdown = shutdown_op };

static void setup(struct tb_nhi *nhi, struct tb_ring *tx, struct tb_ring *rx, bool abandoned)
{
    static struct device dev;

    memset(nhi, 0, sizeof(*nhi));
    memset(events, 0, sizeof(events));
    nevents = 0;
    nhi->dev = &dev;
    nhi->ops = &ops;
    nhi->hop_count = 4;
    nhi->dma_abandoned = abandoned;
    *tx = (struct tb_ring){ .nhi = nhi, .hop = 2, .irq = 102, .is_tx = true, .work = { true, 102 } };
    *rx = (struct tb_ring){ .nhi = nhi, .hop = 3, .irq = 123, .work = { true, 123 } };
    nhi->tx_rings[2] = tx;
    nhi->rx_rings[3] = rx;
}

static void nhi_shutdown(struct tb_nhi *nhi);

static void check(struct tb_nhi *nhi, struct tb_ring *tx, struct tb_ring *rx)
{
    /* Masked first, then each IRQ freed and its work joined, before the
     * NHI's own IRQ teardown (after which devres frees NHI, MMIO, dma_wq). */
    assert(find("mask") == 0);
    assert(find("free_irq 102") > 0 && find("cancel_work 102") > find("free_irq 102"));
    assert(find("free_irq 123") > 0 && find("cancel_work 123") > find("free_irq 123"));
    assert(find("release_nhi_irq") == nevents - 1);
    assert(!tx->irq && !rx->irq && !tx->work.pending && !rx->work.pending);
    /* Nothing reaches them through the NHI any more; the rings, and with
     * them their DMA memory, are not freed. */
    assert(!nhi->tx_rings[2] && !nhi->rx_rings[3] && !tx->freed && !rx->freed);
    assert(!nhi->lock.held);
}

static void abandoned_rings_lose_cpu_paths(void)
{
    struct tb_nhi nhi;
    struct tb_ring tx, rx;

    setup(&nhi, &tx, &rx, true);
    nhi_shutdown(&nhi);
    check(&nhi, &tx, &rx);
    assert(errors == 2 && !warns);
}

static void leaked_rings_lose_cpu_paths(void)
{
    struct tb_nhi nhi;
    struct tb_ring tx, rx;

    setup(&nhi, &tx, &rx, false);
    nhi_shutdown(&nhi);
    check(&nhi, &tx, &rx);
    assert(warns == 2);
}

static void clean_shutdown(void)
{
    struct tb_nhi nhi;
    struct tb_ring tx, rx;

    setup(&nhi, &tx, &rx, false);
    nhi.tx_rings[2] = nhi.rx_rings[3] = NULL;
    nhi_shutdown(&nhi);
    assert(nevents == 2 && find("mask") == 0 && find("release_nhi_irq") == 1 && !warns);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    if (!strcmp(argv[1], "abandoned_rings_lose_cpu_paths"))
        abandoned_rings_lose_cpu_paths();
    else if (!strcmp(argv[1], "leaked_rings_lose_cpu_paths"))
        leaked_rings_lose_cpu_paths();
    else if (!strcmp(argv[1], "clean_shutdown"))
        clean_shutdown();
    else
        return 2;
    printf("PASS %s\n", argv[1]);
    return 0;
}
