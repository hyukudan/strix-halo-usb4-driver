/* SPDX-License-Identifier: GPL-2.0-only */
/* After the header extracts: the NHI and its MMIO as doubles. */
#define trace_tb_latency(...) ((void)0)
struct tb_nhi { spinlock_t lock; struct workqueue_struct *dma_wq; struct workqueue_struct *dma_hi_wq; };
static bool dma_wq_percpu; /* 0044: the module parameter */

/* MMIO: doorbells and interrupt masks are recorded, not performed. */
static int doorbells, last_doorbell, masks;
static void ring_iowrite_prod(struct tb_ring *ring, u16 prod) { (void)ring; ++doorbells; last_doorbell = prod; }
static void ring_iowrite_cons(struct tb_ring *ring, u16 cons) { (void)ring; ++doorbells; last_doorbell = cons; }
static void __ring_interrupt_mask(struct tb_ring *ring, bool mask) { (void)ring; (void)mask; ++masks; }
