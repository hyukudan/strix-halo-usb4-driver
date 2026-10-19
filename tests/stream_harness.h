/* SPDX-License-Identifier: GPL-2.0-only */
/* Userspace doubles for the actual host-path functions of stream.c (ring
 * setup/teardown, NAPI RX reaping, IRQ steering). NOT a hardware emulator:
 * rings, the connection manager's DMA tunnel inventory, IRQ affinity and
 * memory allocation are deterministic models with failure injection. */
#ifndef STREAM_HARNESS_H
#define STREAM_HARNESS_H
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int64_t ktime_t;
typedef u64 dma_addr_t;
typedef int wait_queue_head_t;

#define IS_ENABLED(x) 0
#define BIT(n) (1U << (n))
#define SZ_256 256
#define TB_MAX_FRAME_SIZE 4096
#define GFP_KERNEL 0
#define EPOLLIN 1U
#define EPOLLRDNORM 2U
#define EPOLLOUT 4U
#define EPOLLWRNORM 8U
#define READ_ONCE(x) __atomic_load_n(&(x), __ATOMIC_SEQ_CST)
#define WRITE_ONCE(x, v) __atomic_store_n(&(x), (v), __ATOMIC_SEQ_CST)
#define container_of(p, t, m) ((t *)((char *)(p) - offsetof(t, m)))
#define min(a, b) ((a) < (b) ? (a) : (b))
#define ERR_PTR(e) ((void *)(intptr_t)(e))
#define PTR_ERR(p) ((int)(intptr_t)(p))
#define IS_ERR(p) ((uintptr_t)(p) >= (uintptr_t)-4095)

static atomic_int warnings, infos;
#define pr_warn_ratelimited(...) (atomic_fetch_add(&warnings, 1), (void)0)
#define pr_info_ratelimited(...) (atomic_fetch_add(&infos, 1), (void)0)
#define pr_warn(...) (atomic_fetch_add(&warnings, 1), (void)0)
#define WARN_ON(x) ({ bool __w = (x); if (__w) atomic_fetch_add(&warnings, 1); __w; })

struct list_head { struct list_head *next, *prev; };
static void INIT_LIST_HEAD(struct list_head *l) { l->next = l->prev = l; }
static bool list_empty(const struct list_head *l) { return l->next == l; }
static void list_add_tail(struct list_head *n, struct list_head *h)
{
    n->prev = h->prev; n->next = h; h->prev->next = n; h->prev = n;
}
static void list_del_init(struct list_head *n)
{
    n->prev->next = n->next; n->next->prev = n->prev; INIT_LIST_HEAD(n);
}

/* Mutex double that knows its owner, so lockdep assertions are real. */
struct mutex { pthread_mutex_t m; _Atomic(pthread_t) owner; atomic_bool held; };
static void mutex_init(struct mutex *m) { assert(!pthread_mutex_init(&m->m, NULL)); atomic_store(&m->held, false); }
static void mutex_lock(struct mutex *m)
{
    assert(!pthread_mutex_lock(&m->m));
    atomic_store(&m->owner, pthread_self()); atomic_store(&m->held, true);
}
static int mutex_lock_interruptible(struct mutex *m) { mutex_lock(m); return 0; }
static bool mutex_trylock(struct mutex *m)
{
    if (pthread_mutex_trylock(&m->m))
        return false;
    atomic_store(&m->owner, pthread_self()); atomic_store(&m->held, true);
    return true;
}
static void mutex_unlock(struct mutex *m) { atomic_store(&m->held, false); assert(!pthread_mutex_unlock(&m->m)); }
static bool mutex_held_by_me(const struct mutex *m)
{
    if (!m)
        return false;
    return atomic_load(&m->held) && pthread_equal(atomic_load(&m->owner), pthread_self());
}
#define lockdep_assert_held(m) assert(mutex_held_by_me(m))
typedef struct mutex *class_mutex_t;
static inline class_mutex_t class_mutex_constructor(struct mutex *m) { mutex_lock(m); return m; }
static inline void class_mutex_destructor(class_mutex_t *m) { if (*m) mutex_unlock(*m); }
#define __GUARD_CAT2(a, b) a##b
#define __GUARD_CAT(a, b) __GUARD_CAT2(a, b)
#define guard(name) \
    class_##name##_t __attribute__((cleanup(class_##name##_destructor), unused)) \
    __GUARD_CAT(__guard_, __COUNTER__) = class_##name##_constructor

/* Time, scheduling, signals. */
static ktime_t ktime_get(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (ktime_t)t.tv_sec * 1000000000LL + t.tv_nsec;
}
#define ktime_add_us(t, us) ((t) + (ktime_t)(us) * 1000)
#define ktime_add_ms(t, ms) ((t) + (ktime_t)(ms) * 1000000)
#define ktime_before(a, b) ((a) < (b))
static atomic_bool interrupted;
#define current NULL
static bool signal_pending(void *p) { (void)p; return atomic_load(&interrupted); }
static bool need_resched(void) { return false; }
static void cpu_relax(void) { sched_yield(); }
static void fsleep(unsigned long us) { usleep(us); }
static atomic_int wakeups;
static void wake_up_interruptible_poll(wait_queue_head_t *w, unsigned int m) { (void)w; (void)m; atomic_fetch_add(&wakeups, 1); }

/* Memory: failure injection by call index (0 = next call fails). */
static int kvcalloc_fail_after = -1, page_fail_after = -1, map_fail_after = -1;
static atomic_int kvcalloc_calls, live_arrays, live_pages, live_mappings;
static bool inject(int *counter)
{
    if (*counter == 0) { *counter = -1; return true; }
    if (*counter > 0) --*counter;
    return false;
}
static void *kvcalloc(size_t n, size_t size, int flags)
{
    void *p;
    (void)flags;
    atomic_fetch_add(&kvcalloc_calls, 1);
    if (inject(&kvcalloc_fail_after))
        return NULL;
    p = calloc(n, size);
    if (p) atomic_fetch_add(&live_arrays, 1);
    return p;
}
static void kvfree(void *p) { if (p) { atomic_fetch_sub(&live_arrays, 1); free(p); } }
struct page { unsigned char data[TB_MAX_FRAME_SIZE]; };
static struct page *alloc_page(int flags)
{
    struct page *p;
    (void)flags;
    if (inject(&page_fail_after))
        return NULL;
    p = calloc(1, sizeof(*p));
    if (p) atomic_fetch_add(&live_pages, 1);
    return p;
}
static void __free_page(struct page *p) { atomic_fetch_sub(&live_pages, 1); free(p); }
static void *page_address(struct page *p) { return p->data; }
enum dma_data_direction { DMA_TO_DEVICE, DMA_FROM_DEVICE };
struct device { int id; };
#define DMA_MAPPING_ERROR (~(dma_addr_t)0)
static dma_addr_t dma_map_page(struct device *d, struct page *p, size_t off, size_t size, enum dma_data_direction dir)
{
    (void)d; (void)off; (void)size; (void)dir;
    if (inject(&map_fail_after))
        return DMA_MAPPING_ERROR;
    atomic_fetch_add(&live_mappings, 1);
    return (dma_addr_t)(uintptr_t)p;
}
static int dma_mapping_error(struct device *d, dma_addr_t a) { (void)d; return a == DMA_MAPPING_ERROR; }
static void dma_unmap_page(struct device *d, dma_addr_t a, size_t size, enum dma_data_direction dir)
{
    (void)d; (void)a; (void)size; (void)dir; atomic_fetch_sub(&live_mappings, 1);
}
static void dma_sync_single_for_cpu(struct device *d, dma_addr_t a, size_t s, enum dma_data_direction dir) { (void)d; (void)a; (void)s; (void)dir; }
static void dma_sync_single_for_device(struct device *d, dma_addr_t a, size_t s, enum dma_data_direction dir) { (void)d; (void)a; (void)s; (void)dir; }

/* IRQ affinity: one mask word per Linux IRQ, shared by every ring that gets
 * the same MSI-X vector, like the real reuse after tb_ring_free(). */
struct cpumask { unsigned long bits; };
typedef struct cpumask cpumask_var_t[1];
#define NR_IRQS 256
static struct cpumask irq_masks[NR_IRQS];
static unsigned int nr_cpu_ids = 16;
static unsigned long online_cpus = 0xffff;
static int set_affinity_fail;
static bool cpu_online(int cpu) { return online_cpus & (1UL << cpu); }
static struct cpumask cpu_masks[64];
static const struct cpumask *cpumask_of(int cpu) { cpu_masks[cpu].bits = 1UL << cpu; return &cpu_masks[cpu]; }
static void cpumask_copy(struct cpumask *dst, const struct cpumask *src) { *dst = *src; }
static const struct cpumask *irq_get_affinity_mask(int irq) { return irq > 0 && irq < NR_IRQS ? &irq_masks[irq] : NULL; }
static int irq_set_affinity(int irq, const struct cpumask *m)
{
    if (set_affinity_fail) return set_affinity_fail;
    irq_masks[irq] = *m;
    return 0;
}

/* Rings. Frames move: posted -> done (by complete_rx/complete_tx) -> callback. */
struct tb_ring;
struct ring_frame;
typedef void (*ring_cb)(struct tb_ring *, struct ring_frame *, bool);
struct ring_frame {
    dma_addr_t buffer_phy;
    ring_cb callback;
    struct list_head list;
    u32 size, flags, eof, sof;
};
#define RING_FLAG_NO_INTERRUPT BIT(0)
#define RING_FLAG_FRAME BIT(1)
#define RING_FLAG_E2E BIT(2)
#define RING_DESC_CRC_ERROR 1U
#define RING_DESC_BUFFER_OVERRUN 2U
#define MAX_HOPS 3
struct tb_nhi { struct device dev; struct tb_ring *tx[MAX_HOPS], *rx[MAX_HOPS]; };
struct tb_ring {
    struct tb_nhi *nhi;
    pthread_mutex_t lock;
    int hop, size, irq;
    bool is_tx, running, masked;
    struct list_head posted, done;
    void (*start_poll)(void *);
    void *poll_data;
};
static int ring_fail_after = -1;
static atomic_int live_rings, poll_calls, poll_completes, ring_stops, tx_frames;
static struct tb_ring *ring_alloc(struct tb_nhi *nhi, bool tx, int size)
{
    struct tb_ring **slots = tx ? nhi->tx : nhi->rx;
    struct tb_ring *r;
    int hop;

    if (inject(&ring_fail_after))
        return NULL;
    for (hop = 1; hop < MAX_HOPS && slots[hop]; ++hop)
        ;
    if (hop == MAX_HOPS)
        return NULL;
    r = calloc(1, sizeof(*r));
    assert(r);
    r->nhi = nhi; r->hop = hop; r->size = size; r->is_tx = tx;
    r->irq = (tx ? 100 : 120) + hop;
    pthread_mutex_init(&r->lock, NULL);
    INIT_LIST_HEAD(&r->posted); INIT_LIST_HEAD(&r->done);
    slots[hop] = r;
    atomic_fetch_add(&live_rings, 1);
    return r;
}
static struct tb_ring *tb_ring_alloc_tx(struct tb_nhi *nhi, int hop, int size, unsigned int flags)
{
    (void)hop; (void)flags;
    return ring_alloc(nhi, true, size);
}
static struct tb_ring *tb_ring_alloc_rx(struct tb_nhi *nhi, int hop, int size, unsigned int flags,
                                        int e2e, u16 sof, u16 eof, void (*start_poll)(void *), void *data)
{
    struct tb_ring *r;
    (void)hop; (void)flags; (void)e2e; (void)sof; (void)eof;
    r = ring_alloc(nhi, false, size);
    if (r) { r->start_poll = start_poll; r->poll_data = data; }
    return r;
}
static void tb_ring_start(struct tb_ring *r) { assert(!r->running); r->running = true; }
static void tb_ring_throttling(struct tb_ring *r, unsigned int t) { (void)r; (void)t; }
/* Like the real one: returns once nothing is in flight, else times out.
 * Posted RX buffers stay in flight until data arrives. */
static atomic_int rx_flushes;
static bool tb_ring_flush(struct tb_ring *r, unsigned int ms)
{
    ktime_t end = ktime_add_ms(ktime_get(), ms);
    bool empty;

    if (!r->is_tx)
        atomic_fetch_add(&rx_flushes, 1);
    for (;;) {
        pthread_mutex_lock(&r->lock);
        empty = list_empty(&r->posted);
        pthread_mutex_unlock(&r->lock);
        if (empty || !ktime_before(ktime_get(), end))
            return empty;
        usleep(1000);
    }
}
static void tb_ring_stop(struct tb_ring *r)
{
    struct list_head all;

    assert(r->running); /* a second stop of the same ring is a bug */
    atomic_fetch_add(&ring_stops, 1);
    pthread_mutex_lock(&r->lock);
    r->running = false;
    INIT_LIST_HEAD(&all);
    while (!list_empty(&r->done)) { struct list_head *p = r->done.next; list_del_init(p); list_add_tail(p, &all); }
    while (!list_empty(&r->posted)) { struct list_head *p = r->posted.next; list_del_init(p); list_add_tail(p, &all); }
    pthread_mutex_unlock(&r->lock);
    while (!list_empty(&all)) {
        struct ring_frame *f = container_of(all.next, struct ring_frame, list);
        list_del_init(&f->list);
        f->callback(r, f, true);
    }
}
static void tb_ring_free(struct tb_ring *r)
{
    struct tb_ring **slots = r->is_tx ? r->nhi->tx : r->nhi->rx;

    assert(!r->running && list_empty(&r->posted) && list_empty(&r->done));
    assert(slots[r->hop] == r);
    slots[r->hop] = NULL;
    pthread_mutex_destroy(&r->lock);
    atomic_fetch_sub(&live_rings, 1);
    free(r);
}
static int ring_enqueue(struct tb_ring *r, struct ring_frame *f)
{
    pthread_mutex_lock(&r->lock);
    if (!r->running) { pthread_mutex_unlock(&r->lock); return -ESHUTDOWN; }
    list_add_tail(&f->list, &r->posted);
    pthread_mutex_unlock(&r->lock);
    return 0;
}
/* Audit A4: the controller may complete an RX frame as soon as it is
 * posted (paths and ring already up during open), and the ring work then
 * runs its callback at once. Counts down to the posting that completes. */
static int rx_early_complete_after = -1;
static int tb_ring_rx(struct tb_ring *r, struct ring_frame *f)
{
    int ret;

    assert(!r->is_tx);
    ret = ring_enqueue(r, f);
    if (!ret && inject(&rx_early_complete_after)) {
        pthread_mutex_lock(&r->lock);
        list_del_init(&f->list);
        pthread_mutex_unlock(&r->lock);
        f->size = 64; f->eof = 2; f->sof = 1; f->flags = 0;
        f->callback(r, f, false);
    }
    return ret;
}
static int tx_fail_after = -1;
static int tb_ring_tx(struct tb_ring *r, struct ring_frame *f)
{
    assert(r->is_tx);
    if (inject(&tx_fail_after))
        return -ESHUTDOWN;
    atomic_fetch_add(&tx_frames, 1);
    return ring_enqueue(r, f);
}
/* Test gate: park a polling reader inside tb_ring_poll(). */
static atomic_bool hold_poll, poll_parked;
static struct ring_frame *tb_ring_poll(struct tb_ring *r)
{
    struct ring_frame *f = NULL;

    atomic_fetch_add(&poll_calls, 1);
    while (atomic_load(&hold_poll)) {
        atomic_store(&poll_parked, true);
        sched_yield();
    }
    pthread_mutex_lock(&r->lock);
    if (r->running && !list_empty(&r->done)) {
        f = container_of(r->done.next, struct ring_frame, list);
        list_del_init(&f->list);
    }
    pthread_mutex_unlock(&r->lock);
    return f;
}
static void tb_ring_poll_complete(struct tb_ring *r) { atomic_fetch_add(&poll_completes, 1); r->masked = false; }
static size_t tb_ring_size(const struct tb_ring *r) { return r->size; }
static struct device *tb_ring_dma_device(struct tb_ring *r) { return &r->nhi->dev; }
static size_t tb_ring_frame_size(const struct ring_frame *f) { return f->size ? f->size : TB_MAX_FRAME_SIZE; }
/* The controller completes the oldest posted RX descriptor. */
/* The controller sends the oldest posted TX frame; the ring work runs its
 * callback (interrupt mode). Returns whether there was one. */
static bool complete_tx(struct tb_ring *r)
{
    struct ring_frame *f = NULL;

    /* Under the ring lock, so tb_ring_stop() waits for a running callback
     * like the real one (flush_work) before the frames can be freed. */
    pthread_mutex_lock(&r->lock);
    if (r->running && !list_empty(&r->posted)) {
        f = container_of(r->posted.next, struct ring_frame, list);
        list_del_init(&f->list);
        f->callback(r, f, false);
    }
    pthread_mutex_unlock(&r->lock);
    return f;
}

/* With RING_DESC_CRC_ERROR / RING_DESC_BUFFER_OVERRUN, as the descriptor
 * reports a damaged frame. */
static void complete_rx_flags(struct tb_ring *r, u32 size, u32 flags)
{
    struct ring_frame *f;

    pthread_mutex_lock(&r->lock);
    assert(!list_empty(&r->posted));
    f = container_of(r->posted.next, struct ring_frame, list);
    list_del_init(&f->list);
    f->size = size; f->eof = 2; f->sof = 1; f->flags = flags;
    list_add_tail(&f->list, &r->done);
    pthread_mutex_unlock(&r->lock);
}

static void complete_rx(struct tb_ring *r, u32 size)
{
    struct ring_frame *f;

    pthread_mutex_lock(&r->lock);
    assert(!list_empty(&r->posted));
    f = container_of(r->posted.next, struct ring_frame, list);
    list_del_init(&f->list);
    f->size = size; f->eof = 2; f->sof = 1; f->flags = 0;
    list_add_tail(&f->list, &r->done);
    pthread_mutex_unlock(&r->lock);
}

/* Connection manager DMA tunnel inventory. A tunnel reserves its NHI ring
 * hops and XDomain HopIDs; tb_tunnel_alloc_dma() cannot reserve them twice
 * and tb_approve_xdomain_paths() then returns -ENOMEM without logging. */
struct tb { struct tb_nhi *nhi; };
struct tb_xdomain { struct tb *tb; bool is_unplugged; int hopids; };
struct tb_service { struct tb_xdomain *xd; void *drvdata; struct device dev; };
#define tb_to_service(d) container_of(d, struct tb_service, dev)
#define __maybe_unused __attribute__((unused))
/* Audit A2: a stream may be freed by a concurrent detach, so whatever looks
 * inside it must hold the device mutex. Cases set the mutex to check. */
static struct mutex *stream_deref_lock;
static bool mutex_held_by_me(const struct mutex *m);
static struct tb_xdomain *tb_service_parent(struct tb_service *svc)
{
    assert(!stream_deref_lock || mutex_held_by_me(stream_deref_lock));
    return svc->xd;
}
struct cm_tunnel { struct tb_xdomain *xd; int out_hopid, tx_hop, in_hopid, rx_hop; bool used; };
static struct cm_tunnel tunnels[8];
static int enable_fail;
static int tunnel_count(void)
{
    int n = 0;
    for (int i = 0; i < 8; ++i) n += tunnels[i].used;
    return n;
}
static int tb_xdomain_enable_paths(struct tb_xdomain *xd, int out_hopid, int tx_hop, int in_hopid, int rx_hop)
{
    int i;

    if (enable_fail) { int e = enable_fail; enable_fail = 0; return e; }
    for (i = 0; i < 8; ++i)
        if (tunnels[i].used && tunnels[i].xd->tb->nhi == xd->tb->nhi &&
            (tunnels[i].tx_hop == tx_hop || tunnels[i].rx_hop == rx_hop ||
             (tunnels[i].xd == xd && (tunnels[i].out_hopid == out_hopid || tunnels[i].in_hopid == in_hopid))))
            return -ENOMEM;
    for (i = 0; i < 8 && tunnels[i].used; ++i)
        ;
    assert(i < 8);
    tunnels[i] = (struct cm_tunnel){ xd, out_hopid, tx_hop, in_hopid, rx_hop, true };
    return 0;
}
static int tb_xdomain_disable_paths(struct tb_xdomain *xd, int out_hopid, int tx_hop, int in_hopid, int rx_hop)
{
    /* Like the CM: an unplugged XDomain's tunnels were torn down already. */
    if (xd->is_unplugged)
        return 0;
    for (int i = 0; i < 8; ++i)
        if (tunnels[i].used && tunnels[i].xd == xd && tunnels[i].out_hopid == out_hopid &&
            tunnels[i].tx_hop == tx_hop && tunnels[i].in_hopid == in_hopid && tunnels[i].rx_hop == rx_hop)
            tunnels[i].used = false;
    return 0;
}

/* Remaining types used by the extracted structures. */
struct config_item { const char *ci_name; };
struct config_group { struct config_item cg_item; };
struct miscdevice { const char *name; };
struct kref { int refcount; };
struct tbstream_gpu_context;

/* File side: what read/write/poll/open/release of the stream device use. */
#define ERESTARTSYS 512
#define EPOLLERR 16U
#define EPOLLHUP 32U
#define IOCB_NOWAIT 1
typedef unsigned int __poll_t;
struct inode;
struct poll_table_struct;
struct file { int f_flags; void *private_data; };
struct kiocb { struct file *ki_filp; int ki_flags; };
struct iov_iter { unsigned char *buf; size_t count; };
static size_t iov_iter_count(const struct iov_iter *i) { return i->count; }
/* Audit A8: bytes the user buffer accepts before a fault (-1: no fault). */
static long iov_budget = -1;
static size_t iov_take(size_t size)
{
    size_t n = size;

    if (iov_budget >= 0) {
        n = min(size, (size_t)iov_budget);
        iov_budget -= n;
    }
    return n;
}
static size_t copy_page_to_iter(struct page *p, size_t off, size_t size, struct iov_iter *i)
{
    size = iov_take(size);
    memcpy(i->buf, p->data + off, size); i->buf += size; i->count -= size;
    return size;
}
static size_t copy_page_from_iter(struct page *p, size_t off, size_t size, struct iov_iter *i)
{
    size = iov_take(size);
    memcpy(p->data + off, i->buf, size); i->buf += size; i->count -= size;
    return size;
}
static void iov_iter_revert(struct iov_iter *i, size_t n) { i->buf -= n; i->count += n; }
static void poll_wait(struct file *f, wait_queue_head_t *w, struct poll_table_struct *pt) { (void)f; (void)w; (void)pt; }
static void cond_resched(void) { sched_yield(); }
static void wake_up_interruptible(wait_queue_head_t *w) { (void)w; atomic_fetch_add(&wakeups, 1); }
/* Sleeps by polling the condition, which other test threads change. */
#define wait_event_interruptible(wq, cond) ({     int __ret = 0;     (void)(wq);     while (!(cond)) {         if (signal_pending(current)) { __ret = -ERESTARTSYS; break; }         sched_yield();     }     __ret; })
#define scoped_guard(name, arg)     for (class_##name##_t __scope __attribute__((cleanup(class_##name##_destructor))) =          class_##name##_constructor(arg), *__scope_done = NULL; !__scope_done;          __scope_done = (void *)1)

/* Lists and configfs, as used by the device and group items. */
#define list_entry(p, t, m) container_of(p, t, m)
#define list_for_each_entry(pos, head, member)     for (pos = list_entry((head)->next, __typeof__(*pos), member);          &pos->member != (head); pos = list_entry(pos->member.next, __typeof__(*pos), member))
#define list_for_each_entry_reverse(pos, head, member)     for (pos = list_entry((head)->prev, __typeof__(*pos), member);          &pos->member != (head); pos = list_entry(pos->member.prev, __typeof__(*pos), member))
static void list_del(struct list_head *n) { list_del_init(n); }
#define to_config_group(item) container_of(item, struct config_group, cg_item)
static const char *config_item_name(struct config_item *item) { return item->ci_name; }
static atomic_int item_puts, group_puts;
static void config_item_put(struct config_item *item) { (void)item; atomic_fetch_add(&item_puts, 1); }
static void config_group_put(struct config_group *group) { (void)group; atomic_fetch_add(&group_puts, 1); }

/* Reference counts are counted, objects are owned by the test. */
struct tbstream;
struct tbstream_dev;
struct tbstream_group;
static atomic_int stream_refs, dev_refs;
static struct tbstream *tbstream_get(struct tbstream *s) { if (s) atomic_fetch_add(&stream_refs, 1); return s; }
static void tbstream_put(struct tbstream *s) { if (s) assert(atomic_fetch_sub(&stream_refs, 1) > 0); }
static struct tbstream_dev *tbstream_dev_get(struct tbstream_dev *s) { atomic_fetch_add(&dev_refs, 1); return s; }
static void tbstream_dev_put(struct tbstream_dev *s) { (void)s; assert(atomic_fetch_sub(&dev_refs, 1) > 0); }
static struct tbstream_group *found_group;
static struct tbstream_group *tbstream_group_find(struct tbstream *s) { (void)s; return found_group; }

/* XDomain HopIDs and the service property directory. */
static int tb_xdomain_alloc_in_hopid(struct tb_xdomain *xd, int hopid) { xd->hopids++; return hopid; }
static int tb_xdomain_alloc_out_hopid(struct tb_xdomain *xd, int hopid) { xd->hopids++; return hopid; }
static void tb_xdomain_release_in_hopid(struct tb_xdomain *xd, int hopid) { (void)hopid; assert(--xd->hopids >= 0); }
static void tb_xdomain_release_out_hopid(struct tb_xdomain *xd, int hopid) { (void)hopid; assert(--xd->hopids >= 0); }
static int published_in = -1, published_out = -1;
static void service_get_hopids(struct tb_service *svc, const char *name, int *in, int *out)
{
    (void)svc; (void)name; (void)in; (void)out;
}
static int service_update_properties(struct tb_service *svc, const char *name, int in, int out)
{
    (void)svc; (void)name; published_in = in; published_out = out;
    return 0;
}
static void tb_service_properties_changed(struct tb_service *svc) { (void)svc; }
static void *tb_service_get_drvdata(struct tb_service *svc) { return svc->drvdata; }

/* Module parameters of stream.c (declared by module_param there). */
static bool rx_napi;
static unsigned int spin_us;
static unsigned int open_timeout_ms = 30000;
/* Module parameter plumbing for the spin_us setter. */
struct kernel_param { unsigned int *arg; };
static int kstrtouint(const char *s, unsigned int base, unsigned int *res)
{
    char *end;
    unsigned long v;

    errno = 0;
    v = strtoul(s, &end, base);
    if (errno || end == s || (*end && *end != '\n') || v > 0xffffffffUL)
        return -EINVAL;
    *res = v;
    return 0;
}
static int param_set_uint(const char *s, const struct kernel_param *kp)
{
    return kstrtouint(s, 0, kp->arg);
}

/* Bounded open (0019): time in jiffies of 1 ms, the timed wait, and the
 * core's answer about the endpoint's domain and XDomain. */
#define HZ 1000
#define msecs_to_jiffies(ms) ((unsigned long)(ms))
#define jiffies ((unsigned long)(ktime_get() / 1000000))
#define min_t(t, a, b) ((t)(a) < (t)(b) ? (t)(a) : (t)(b))
#define wait_event_interruptible_timeout(wq, cond, timeout) ({ \
    long __left = (timeout); \
    ktime_t __end = ktime_add_ms(ktime_get(), __left); \
    (void)(wq); \
    for (;;) { \
        if (cond) { __left = __left > 0 ? __left : 1; break; } \
        if (signal_pending(current)) { __left = -ERESTARTSYS; break; } \
        if (!ktime_before(ktime_get(), __end)) { __left = (cond) ? 1 : 0; break; } \
        sched_yield(); \
    } \
    __left; })
#define wait_event_timeout(wq, cond, timeout) ({ \
    long __left = (timeout); \
    ktime_t __end = ktime_add_ms(ktime_get(), __left); \
    (void)(wq); \
    for (;;) { \
        if (cond) { __left = __left > 0 ? __left : 1; break; } \
        if (!ktime_before(ktime_get(), __end)) { __left = (cond) ? 1 : 0; break; } \
        sched_yield(); \
    } \
    __left; })
static atomic_int presence_result = -ENXIO, presence_calls;
static int tb_xdomain_presence(unsigned int domain, u64 route)
{
    assert(domain == 1 && route == 2); /* the endpoint group "1-2.0" */
    atomic_fetch_add(&presence_calls, 1);
    return atomic_load(&presence_result);
}
#endif
