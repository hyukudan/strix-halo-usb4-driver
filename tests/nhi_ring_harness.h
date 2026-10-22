/* SPDX-License-Identifier: GPL-2.0-only */
/* Kernel model for the ring completion path of nhi.c: the actual
 * ring_write_descriptors(), ring_work(), tb_ring_poll() and __ring_interrupt()
 * (and the GPU polling collector) run against these doubles. The ring,
 * frame and descriptor layouts come from the patched headers; MMIO, the
 * workqueue and the clock are models (the clock advances by clock_step per
 * read, so collection durations are checkable). */
#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef uint64_t dma_addr_t;
#define __packed __attribute__((packed))
#define IS_ENABLED(x) 0
#define BIT(n) (1U << (n))
#define container_of(p, t, m) ((t *)((char *)(p) - offsetof(t, m)))
#define READ_ONCE(x) (x)
static int warnings;
#define WARN_ON_ONCE(c) ({ bool __c = (c); if (__c) ++warnings; __c; })

struct list_head { struct list_head *next, *prev; };
static void INIT_LIST_HEAD(struct list_head *l) { l->next = l->prev = l; }
static bool list_empty(const struct list_head *l) { return l->next == l; }
static void __list_del(struct list_head *e) { e->prev->next = e->next; e->next->prev = e->prev; }
static void list_add_tail(struct list_head *e, struct list_head *h)
{ e->prev = h->prev; e->next = h; h->prev->next = e; h->prev = e; }
static void list_del_init(struct list_head *e) { __list_del(e); INIT_LIST_HEAD(e); }
static void list_move_tail(struct list_head *e, struct list_head *h) { __list_del(e); list_add_tail(e, h); }
static void list_splice_tail_init(struct list_head *l, struct list_head *h)
{
    while (!list_empty(l))
        list_move_tail(l->next, h);
}
static size_t list_count_nodes(const struct list_head *h)
{
    size_t n = 0;

    for (const struct list_head *p = h->next; p != h; p = p->next)
        ++n;
    return n;
}
#define LIST_HEAD(n) struct list_head n = { &(n), &(n) }
#define list_first_entry(h, t, m) container_of((h)->next, t, m)
#define list_for_each_entry_safe(pos, n, head, member) \
    for (pos = container_of((head)->next, __typeof__(*pos), member), \
         n = container_of(pos->member.next, __typeof__(*pos), member); \
         &pos->member != (head); \
         pos = n, n = container_of(n->member.next, __typeof__(*n), member))

typedef struct { pthread_mutex_t m; int held; } spinlock_t;
static void spin_lock_init(spinlock_t *l) { pthread_mutex_init(&l->m, NULL); l->held = 0; }
static void spin_lock(spinlock_t *l) { pthread_mutex_lock(&l->m); l->held = 1; }
static void spin_unlock(spinlock_t *l) { l->held = 0; pthread_mutex_unlock(&l->m); }
#define spin_lock_irqsave(l, f) do { (void)(f); spin_lock(l); } while (0)
#define spin_unlock_irqrestore(l, f) do { (void)(f); spin_unlock(l); } while (0)

struct work_struct { int queued; };
struct workqueue_struct { int unused; };
static int works_queued;
static struct workqueue_struct *last_wq;
static bool queue_work(struct workqueue_struct *wq, struct work_struct *w)
{
    last_wq = wq;
    ++works_queued;
    if (w->queued)
        return false;
    w->queued = 1;
    return true;
}
typedef struct { int wakeups; } wait_queue_head_t;
static void wake_up(wait_queue_head_t *w) { ++w->wakeups; }
static void dma_wmb(void) { }

static u64 fake_now, clock_step = 1;
static u64 ktime_get_ns(void) { return fake_now += clock_step; }
