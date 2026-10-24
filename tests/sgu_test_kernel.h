/* SPDX-License-Identifier: GPL-2.0-only */
/* Deterministic userspace kernel-API simulator, NOT a hardware/kernel build. */
#ifndef SGU_TEST_KERNEL_H
#define SGU_TEST_KERNEL_H
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <linux/types.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <time.h>
#include <unistd.h>
#include <linux/strix_gpu_stream.h>

struct rcu_head { int unused; };
static pthread_rwlock_t test_rcu_lock = PTHREAD_RWLOCK_INITIALIZER;
static void rcu_read_lock(void) { assert(!pthread_rwlock_rdlock(&test_rcu_lock)); }
static void rcu_read_unlock(void) { assert(!pthread_rwlock_unlock(&test_rcu_lock)); }
/* Test-only synchronous grace period. Real kfree_rcu is asynchronous. */
static void test_kfree_rcu(void *p) { assert(!pthread_rwlock_wrlock(&test_rcu_lock)); free(p); assert(!pthread_rwlock_unlock(&test_rcu_lock)); }
#define kfree_rcu(ptr, field) test_kfree_rcu(ptr)
#define TB_DMA_DOMAIN_QUIESCING 1UL
#define TB_DMA_DOMAIN_DMA_REVOKED 3UL
#define TB_DMA_DOMAIN_ABANDONED 4UL
#define TB_DMA_RESET_WITH_NETWORK 1U
#define NOTIFY_DONE 0
#define NOTIFY_OK 1
#define NOTIFY_BAD 2
struct notifier_block { int (*notifier_call)(struct notifier_block *, unsigned long, void *); };
static struct notifier_block *test_notifier;
static int tb_register_dma_lifecycle_notifier(struct notifier_block *n) { assert(!test_notifier); test_notifier=n; return 0; }
static int tb_unregister_dma_lifecycle_notifier(struct notifier_block *n) { assert(test_notifier==n); test_notifier=NULL; return 0; }
typedef __u64 u64;
typedef __u32 u32;
typedef __u8 u8;
typedef u64 dma_addr_t;
typedef unsigned int __poll_t;
typedef int poll_table;
#define __user
#define GFP_KERNEL 0
static _Thread_local unsigned signalling_depth;
static bool dma_fence_begin_signalling(void) { ++signalling_depth; return true; }
static void dma_fence_end_signalling(bool cookie) { assert(cookie && signalling_depth); --signalling_depth; }
#define lockdep_assert_held(lock) ((void)(lock))
#define CAP_SYS_ADMIN 21
static bool admin = true;
static bool capable(int capability) { assert(capability == CAP_SYS_ADMIN); return admin; }
#define WQ_UNBOUND 1
#define WQ_MEM_RECLAIM 2
struct workqueue_struct { int placeholder; };
static int workqueues;
static struct workqueue_struct *alloc_workqueue(const char *name, unsigned flags, int n) {
    (void)name; (void)n; assert((flags & WQ_MEM_RECLAIM) && !signalling_depth);
    struct workqueue_struct *w = calloc(1, sizeof(*w)); if (w) ++workqueues; return w;
}
static void destroy_workqueue(struct workqueue_struct *w) { assert(w && workqueues > 0); --workqueues; free(w); }
#define NSEC_PER_MSEC 1000000ULL
#define U64_MAX (~(u64)0)
#define ERESTARTSYS 512
#define BIT(n) (1U << (n))
#define min_t(t,a,b) ((t)(a) < (t)(b) ? (t)(a) : (t)(b))
#ifndef max
#define max(a,b) ((a) > (b) ? (a) : (b))
#define min(a, b) ((a) < (b) ? (a) : (b))
#endif
#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif
#ifndef PAGE_SIZE
#define PAGE_SIZE 4096UL
#endif
typedef int64_t s64;
#define NSEC_PER_USEC 1000ULL
static u64 div_u64(u64 a, u32 b) { return a / b; }
static u64 div64_u64(u64 a, u64 b) { return a / b; }
static int fls64(u64 x) { return x ? 64 - __builtin_clzll(x) : 0; }
#define scnprintf(buf, size, ...) ({ size_t __s = (size); int __n = __s ? snprintf((buf), __s, __VA_ARGS__) : 0; \
    __n < 0 ? 0 : (size_t)__n >= __s ? (int)(__s ? __s - 1 : 0) : __n; })
#define sysfs_emit(buf, ...) scnprintf((buf), PAGE_SIZE, __VA_ARGS__)
#define READ_ONCE(x) __atomic_load_n(&(x), __ATOMIC_SEQ_CST)
#define WRITE_ONCE(x,v) __atomic_store_n(&(x), (v), __ATOMIC_SEQ_CST)
#define container_of(p,t,m) ((t *)((char *)(p) - offsetof(t,m)))
#include <stddef.h>
#define MODULE_IMPORT_NS(...)
#define EXPORT_SYMBOL_GPL(x)
#define MODULE_PARM_DESC(...)
#define module_param_cb(...)
#define module_param(...)
#define clamp_val(v,lo,hi) ((v) < (lo) ? (lo) : (v) > (hi) ? (hi) : (v))
#define THIS_MODULE ((void *)1)
#define WARN_ON_ONCE(x) (!!(x))
#define dev_warn(...) ((void)0)
static atomic_int module_refs;
static void __module_get(void *m) { (void)m; atomic_fetch_add(&module_refs, 1); }
static void module_put(void *m) { (void)m; assert(atomic_fetch_sub(&module_refs, 1) > 0); }
struct kernel_param { void *arg; };
struct kernel_param_ops { int (*set)(const char *, const struct kernel_param *); int (*get)(char *, const struct kernel_param *); };
static int param_get_uint(char *out, const struct kernel_param *p) { return sprintf(out, "%u", *(unsigned *)p->arg); }
static int kstrtouint(const char *s, unsigned int base, unsigned int *out) {
    char *end; unsigned long v; errno=0;
    if (*s=='-') return -EINVAL;
    v=strtoul(s,&end,base);
    if (errno || v>UINT_MAX) return -ERANGE;
    if (*end && *end!='\n') return -EINVAL;
    *out=v; return 0;
}
static int kstrtobool(const char *s, bool *out) {
    if (!s || !*s) return -EINVAL;
    if (*s=='1'||*s=='y'||*s=='Y') { *out=true; return 0; }
    if (*s=='0'||*s=='n'||*s=='N') { *out=false; return 0; }
    return -EINVAL;
}
static int alloc_fail_after=-1;
static void (*allocation_hook)(void);
static void *kzalloc(size_t n, int flags) {
    (void)flags;
    assert(!signalling_depth);
    if (allocation_hook) allocation_hook();
    if (alloc_fail_after==0) { alloc_fail_after=-1; return NULL; }
    if (alloc_fail_after>0) --alloc_fail_after;
    return calloc(1,n);
}
static void *kvcalloc(size_t n, size_t sz, int flags) {
    if (sz && n>SIZE_MAX/sz) return NULL;
    return kzalloc(n*sz,flags);
}
#define kfree free
#define kvfree free
#define ERR_PTR(e) ((void *)(intptr_t)(e))
#define PTR_ERR(p) ((int)(intptr_t)(p))
#define IS_ERR(p) ((uintptr_t)(p) >= (uintptr_t)-4095)
static void *memchr_inv(const void *p, int c, size_t n) {
    const unsigned char *s=p;
    while(n--) { if (*s!=(unsigned char)c) return (void *)s; ++s; }
    return NULL;
}

struct mutex { pthread_mutex_t m; };
#define DEFINE_MUTEX(name) struct mutex name = { PTHREAD_MUTEX_INITIALIZER }
static void mutex_init(struct mutex *m) { assert(!pthread_mutex_init(&m->m,NULL)); }
static void mutex_lock(struct mutex *m) { assert(!pthread_mutex_lock(&m->m)); }
static int mutex_lock_interruptible(struct mutex *m) { mutex_lock(m); return 0; }
static void mutex_unlock(struct mutex *m) { assert(!pthread_mutex_unlock(&m->m)); }
/* Fault injection: the trylock of trylock_fail fails once, after
 * trylock_fail_skip successful ones (a holder appearing in between). */
static struct mutex *trylock_fail;
static int trylock_fail_skip;
static bool mutex_trylock(struct mutex *m)
{
    if (m == trylock_fail && trylock_fail_skip-- == 0) {
        trylock_fail = NULL;
        return false;
    }
    return !pthread_mutex_trylock(&m->m);
}
typedef struct mutex spinlock_t;
#define spin_lock_init mutex_init
#define spin_lock_irqsave(l,f) do { (f)=0; mutex_lock(l); } while (0)
#define spin_unlock_irqrestore(l,f) do { (void)(f); mutex_unlock(l); } while(0)

struct list_head { struct list_head *next,*prev; };
#define list_for_each_entry(pos, head, member) \
    for (pos = container_of((head)->next, __typeof__(*pos), member); \
         &pos->member != (head); \
         pos = container_of(pos->member.next, __typeof__(*pos), member))
#define LIST_HEAD(name) struct list_head name={ &name,&name }
static void INIT_LIST_HEAD(struct list_head *l) { l->next=l->prev=l; }
static bool list_empty(const struct list_head *l) { return l->next==l; }
static void list_add_tail(struct list_head *n,struct list_head *h) { n->prev=h->prev;n->next=h;h->prev->next=n;h->prev=n; }
static void list_del_init(struct list_head *n) { n->prev->next=n->next;n->next->prev=n->prev;INIT_LIST_HEAD(n); }

/* The monotonic clock; a test may drive it instead (fake_clock): then it
 * moves 1 ns per read and by clock_advance_ms(), never by descheduling. */
static bool fake_clock;
static u64 fake_clock_ns;
static u64 ktime_get_ns(void) {
    if(__atomic_load_n(&fake_clock,__ATOMIC_SEQ_CST))return __atomic_add_fetch(&fake_clock_ns,1,__ATOMIC_SEQ_CST);
    struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);return (u64)t.tv_sec*1000000000ULL+t.tv_nsec;
}
static void clock_advance_ms(u64 ms) { __atomic_add_fetch(&fake_clock_ns,ms*NSEC_PER_MSEC,__ATOMIC_SEQ_CST); }
static void clock_fake(bool on) { if(on)fake_clock_ns=ktime_get_ns(); __atomic_store_n(&fake_clock,on,__ATOMIC_SEQ_CST); }
#define jiffies ((unsigned long)(ktime_get_ns()/NSEC_PER_MSEC))
#define msecs_to_jiffies(x) ((unsigned long)(x))
#define time_after_eq(a,b) ((long)((a)-(b))>=0)
static atomic_bool interrupted;
#define current NULL
static bool signal_pending(void *p) { (void)p; return atomic_load(&interrupted); }
static long schedule_timeout_interruptible(long ticks) { usleep((useconds_t)ticks*1000U); return 0; }
struct work_struct { void (*fn)(struct work_struct *); };
struct delayed_work { struct work_struct work; atomic_bool scheduled; unsigned long due; };
#define system_wq NULL
#define to_delayed_work(w) container_of(w, struct delayed_work, work)
#define INIT_DELAYED_WORK(w,func) do { (w)->work.fn=(func); atomic_init(&(w)->scheduled,false); } while (0)
static int mod_delayed_work(void *q, struct delayed_work *w, unsigned long ticks) {
    (void)q; w->due=jiffies+ticks; atomic_store(&w->scheduled,true);return 0;
}
static bool queue_delayed_work(void *q, struct delayed_work *w, unsigned long ticks) {
    (void)q;
    if (atomic_exchange(&w->scheduled,true)) return false;
    w->due=jiffies+ticks;return true;
}
static bool cancel_delayed_work(struct delayed_work *w) { return atomic_exchange(&w->scheduled,false); }
static bool cancel_delayed_work_sync(struct delayed_work *w) { return cancel_delayed_work(w); }
static void run_delayed_work(struct delayed_work *w) {
    if (atomic_exchange(&w->scheduled,false)) w->work.fn(&w->work);
}
static int wakeups;
static void wake_up_interruptible_poll(int *w, unsigned m) { (void)w;(void)m;++wakeups; }
static int copy_from_fail,copy_to_fail;
static int copy_from_user(void *a,const void *b,size_t n) { if(copy_from_fail){--copy_from_fail;return 1;}memcpy(a,b,n);return 0; }
static int copy_to_user(void *a,const void *b,size_t n) { if(copy_to_fail){--copy_to_fail;return 1;}memcpy(a,b,n);return 0; }

struct dma_fence;
struct dma_fence_ops {
    const char *(*get_driver_name)(struct dma_fence *);
    const char *(*get_timeline_name)(struct dma_fence *);
    void (*release)(struct dma_fence *);
};
struct dma_fence {
    atomic_int refs;
    atomic_bool signaled;
    int error;
    u64 context,seqno;
    const struct dma_fence_ops *ops;
};
static atomic_int fence_waiters;
static void dma_fence_init(struct dma_fence *f, const struct dma_fence_ops *ops, spinlock_t *lock,u64 context,u64 seq) {
    (void)lock; atomic_init(&f->refs,1);atomic_init(&f->signaled,false);f->error=0;f->context=context;f->seqno=seq;f->ops=ops;
}
static struct dma_fence *dma_fence_get(struct dma_fence *f) { if(f)assert(atomic_fetch_add(&f->refs,1)>0);return f; }
static void dma_fence_free(struct dma_fence *f) { free(f); }
static void dma_fence_put(struct dma_fence *f) { if(f&&atomic_fetch_sub(&f->refs,1)==1)f->ops->release(f); }
static bool dma_fence_is_signaled(struct dma_fence *f) { return atomic_load(&f->signaled); }
static void dma_fence_set_error(struct dma_fence *f,int err) { assert(!dma_fence_is_signaled(f));f->error=err; }
static void (*signal_hook)(struct dma_fence *);
static int dma_fence_signal(struct dma_fence *f) {
    if (signal_hook) signal_hook(f);
    return atomic_exchange(&f->signaled,true)?-EINVAL:0;
}
static long dma_fence_wait_timeout(struct dma_fence *f,bool interruptible,unsigned long ticks) {
    unsigned long until=jiffies+ticks;long ret=0;(void)interruptible;
    atomic_fetch_add(&fence_waiters,1);
    while(!time_after_eq(jiffies,until)) {
        if(signal_pending(NULL)){ret=-ERESTARTSYS;break;}
        if(dma_fence_is_signaled(f)){ret=1;break;}
        usleep(500);
    }
    atomic_fetch_sub(&fence_waiters,1);return ret;
}
static u64 next_fence_context=100;
static u64 dma_fence_context_alloc(unsigned n) { u64 v=next_fence_context;next_fence_context+=n;return v; }
enum dma_resv_usage { DMA_RESV_USAGE_KERNEL, DMA_RESV_USAGE_WRITE, DMA_RESV_USAGE_READ, DMA_RESV_USAGE_BOOKKEEP };
struct dma_resv { struct mutex lock; unsigned count; struct dma_fence *fences[64];enum dma_resv_usage usages[64]; };
struct dma_resv_iter { struct dma_resv *resv; unsigned index;enum dma_resv_usage usage; };
static bool dma_resv_trylock(struct dma_resv *r) { assert(!signalling_depth); return mutex_trylock(&r->lock); }
static void dma_resv_unlock(struct dma_resv *r) { mutex_unlock(&r->lock); }
static int reserve_fail;
static int dma_resv_reserve_fences(struct dma_resv *r,unsigned n) { (void)n;if(reserve_fail){reserve_fail=0;return -ENOMEM;}return r->count<64?0:-ENOMEM; }
static struct dma_fence *resv_next(struct dma_resv_iter *c) {
    while(c->index<c->resv->count) {
        unsigned i=c->index++;
        if(c->resv->usages[i]<=c->usage)return c->resv->fences[i];
    }
    return NULL;
}
#define dma_resv_for_each_fence(c,r,u,f) \
    for (*(c)=(struct dma_resv_iter){ .resv=(r), .usage=(u), .index=0 },(f)=resv_next(c);(f);(f)=resv_next(c))
static void dma_resv_add_fence(struct dma_resv *r,struct dma_fence *f,enum dma_resv_usage usage) {
    unsigned i;
    for(i=0;i<r->count;++i)if(r->fences[i]->context==f->context)break;
    if(i<r->count)dma_fence_put(r->fences[i]);else ++r->count;
    r->fences[i]=dma_fence_get(f);r->usages[i]=usage;
}
static void resv_clear(struct dma_resv *r) { for(unsigned i=0;i<r->count;++i)dma_fence_put(r->fences[i]);r->count=0; }

struct device { unsigned id; bool bound, locked; void *driver_data; };
static bool device_lock_busy;
static bool device_trylock(struct device *d) { if (device_lock_busy || d->locked) return false; d->locked=true; return true; }
static void device_unlock(struct device *d) { assert(d->locked);d->locked=false; }
static bool device_is_bound(struct device *d) { assert(d->locked);return d->bound; }
static void *dev_get_drvdata(struct device *d) { assert(d->locked);return d->driver_data; }
static struct device *get_device(struct device *d) { return d; }
static void put_device(struct device *d) { (void)d; }
struct super_block { u64 s_dev; };
struct inode { struct super_block *i_sb;u64 i_ino; };
struct sync_file;
struct file_operations { int value; };
static const struct file_operations tbstream_dev_fops = {1};
struct file { const struct file_operations *f_op; unsigned int borrowed; void *private_data;unsigned f_flags;struct inode *inode;struct sync_file *sync; };
struct sync_file { struct file *file;struct dma_fence *fence; };
static struct file *fd_table[256];
static bool fd_reserved[256];
static int fd_fail;
static int get_unused_fd_flags(unsigned flags) {
    assert(flags==O_CLOEXEC);if(fd_fail){fd_fail=0;return -EMFILE;}
    for(int i=10;i<256;++i)if(!fd_reserved[i]&&!fd_table[i]){fd_reserved[i]=true;return i;}
    return -EMFILE;
}
static void put_unused_fd(int fd) { assert(fd_reserved[fd]);fd_reserved[fd]=false; }
static void fd_install(int fd,struct file *f) { assert(fd_reserved[fd]);fd_reserved[fd]=false;fd_table[fd]=f;f->f_flags=O_CLOEXEC; }
static struct sync_file *sync_file_create(struct dma_fence *f) {
    struct sync_file *s=kzalloc(sizeof(*s),0);if(!s)return NULL;
    s->file=calloc(1,sizeof(*s->file));assert(s->file);s->file->sync=s;s->fence=dma_fence_get(f);return s;
}
static struct file *fget(int fd) { if (fd < 0 || fd >= 256 || !fd_table[fd]) return NULL; ++fd_table[fd]->borrowed; return fd_table[fd]; }
static void fput(struct file *f) { if (!f->sync) { assert(f->borrowed); --f->borrowed; return; } assert(f->sync);dma_fence_put(f->sync->fence);free(f->sync);free(f); }
static struct inode *file_inode(struct file *f) { return f->inode; }
static u64 huge_encode_dev(u64 d) { return d; }
enum dma_data_direction { DMA_TO_DEVICE,DMA_FROM_DEVICE };
struct scatterlist { dma_addr_t address;u64 bytes;bool bounce; };
struct sg_table { unsigned nents;struct scatterlist *sgl; };
struct dma_buf { u64 size;struct file *file;struct dma_resv *resv;atomic_int refs;struct sg_table *mapping; };
struct dma_buf_attachment { struct dma_buf *buffer;struct device *dev; };
static struct dma_buf *dma_buffers[16];
static int unmaps,detaches,dma_syncs;
static bool need_sync;
static struct dma_buf *dma_buf_get(int fd) { if(fd<0||fd>=16||!dma_buffers[fd])return ERR_PTR(-EBADF);atomic_fetch_add(&dma_buffers[fd]->refs,1);return dma_buffers[fd]; }
static void dma_buf_put(struct dma_buf *b) { assert(atomic_fetch_sub(&b->refs,1)>1); }
static struct dma_buf_attachment *dma_buf_attach(struct dma_buf *b,struct device *d) { struct dma_buf_attachment *a=calloc(1,sizeof(*a));a->buffer=b;a->dev=d;return a; }
static void dma_buf_detach(struct dma_buf *b,struct dma_buf_attachment *a) { (void)b;++detaches;free(a); }
static struct sg_table *dma_buf_map_attachment_unlocked(struct dma_buf_attachment *a,enum dma_data_direction d) { (void)d;return a->buffer->mapping; }
static void dma_buf_unmap_attachment_unlocked(struct dma_buf_attachment *a,struct sg_table *t,enum dma_data_direction d) { (void)a;(void)t;(void)d;++unmaps; }
static void dma_sync_sgtable_for_device(struct device *d,struct sg_table *t,enum dma_data_direction dir) { (void)d;(void)t;(void)dir;++dma_syncs; }
#define for_each_sg(sglist,sg,count,i) for ((i)=0,(sg)=(sglist);(i)<(count);++(i),++(sg))
#define sg_dma_len(sg) ((sg)->bytes)
#define sg_dma_address(sg) ((sg)->address)
#define sg_dma_is_swiotlb(sg) ((sg)->bounce)
static u64 dma_get_mask(struct device *d) { (void)d;return U64_MAX; }
/* Kernel pages for kernel-validated terminal frames. */
#define __GFP_ZERO 0x100
struct page { unsigned char data[4096]; };
static int page_fail, live_pages, page_syncs;
static struct page *alloc_page(int flags) {
    assert(flags & __GFP_ZERO); assert(!signalling_depth);
    if (page_fail) { page_fail=0; return NULL; }
    ++live_pages; return calloc(1, sizeof(struct page));
}
static void __free_page(struct page *p) { assert(live_pages>0); --live_pages; free(p); }
static void *page_address(struct page *p) { return p->data; }
static dma_addr_t dma_map_page(struct device *d,struct page *p,size_t o,size_t n,enum dma_data_direction dir) { (void)d;(void)o;(void)n;(void)dir;return (dma_addr_t)(uintptr_t)p; }
static int dma_mapping_error(struct device *d,dma_addr_t a) { (void)d;return !a; }
static void dma_unmap_page(struct device *d,dma_addr_t a,size_t n,enum dma_data_direction dir) { (void)d;(void)a;(void)n;(void)dir; }
static void dma_sync_single_for_cpu(struct device *d,dma_addr_t a,size_t n,enum dma_data_direction dir) { (void)d;(void)a;(void)n;(void)dir;++page_syncs; }
static void dma_sync_single_for_device(struct device *d,dma_addr_t a,size_t n,enum dma_data_direction dir) { (void)d;(void)a;(void)n;(void)dir;++page_syncs; }
static int terminal_warnings;
#define pr_warn_ratelimited(...) (++terminal_warnings, (void)0)
static bool dma_need_sync(struct device *d,dma_addr_t a) { (void)d;(void)a;return need_sync; }

typedef struct { unsigned char b[16]; } uuid_t;
static void uuid_gen(uuid_t *u) { static u32 n=1;memset(u,0,sizeof(*u));memcpy(u->b,&n,sizeof(n));++n; }
struct tb_ring;
struct ring_frame { struct list_head list;dma_addr_t buffer_phy;unsigned flags,sof,eof,size;void (*callback)(struct tb_ring *,struct ring_frame *,bool); };
struct tb_nhi { struct device dev; bool going_away, dma_shutdown; };
struct tb_ring_stamps { u64 irq_ns, start_ns, reap_ns; };
struct tb_ring { struct work_struct work; spinlock_t lock;bool running,is_tx;unsigned hop,size;struct list_head queue;struct tb_nhi *nhi;
    struct tb_ring_stamps cb_stamps; u64 irqs, collections, collect_ns_total, collect_ns_max;
    unsigned flags; struct list_head done; u64 polls; };
#define RING_DESC_CRC_ERROR 1U
#define RING_DESC_BUFFER_OVERRUN 2U
#define RING_FLAG_FRAME 1U
#define RING_FLAG_E2E 2U
#define RING_FLAG_NO_INTERRUPT 8U
#define TBSTREAM_FRAME_START 1U
#define TBSTREAM_DATA 2U
static unsigned tb_ring_frame_size(struct ring_frame *f) { return f->size?f->size:4096; }
static struct device *tb_ring_dma_device(struct tb_ring *r) { return &r->nhi->dev; }
static struct tb_ring *tb_ring_alloc_tx(struct tb_nhi *nhi,int hop,unsigned size,unsigned flags) {
    struct tb_ring *r=calloc(1,sizeof(*r));assert(r);(void)hop;r->flags=flags;
    spin_lock_init(&r->lock);r->nhi=nhi;r->is_tx=true;r->hop=2;r->size=size;INIT_LIST_HEAD(&r->queue);INIT_LIST_HEAD(&r->done);return r;
}
static struct tb_ring *tb_ring_alloc_rx(struct tb_nhi *nhi,int hop,unsigned size,unsigned flags,int tx_hop,unsigned sof,unsigned eof,void *cb,void *data) {
    struct tb_ring *r=tb_ring_alloc_tx(nhi,hop,size,flags);(void)tx_hop;(void)sof;(void)eof;(void)cb;(void)data;r->is_tx=false;r->hop=3;return r;
}
static void tb_ring_start(struct tb_ring *r) { r->running=true; }
static bool stop_fails;
static void tb_ring_stop(struct tb_ring *r) {
    if(stop_fails)return;
    r->running=false;
    /* Finished but not collected (polled rings) are canceled like the rest. */
    while(!list_empty(&r->done)) {
        struct ring_frame *f=container_of(r->done.next,struct ring_frame,list);
        list_del_init(&f->list);f->callback(r,f,true);
    }
    while(!list_empty(&r->queue)) {
        struct ring_frame *f=container_of(r->queue.next,struct ring_frame,list);
        list_del_init(&f->list);f->callback(r,f,true);
    }
}
/* The polled collector: frames the "controller" finished (r->done), in
 * order, at most @budget, callbacks never canceled. A test may require a
 * mutex (dma_lock) to be held by the caller. */
static struct mutex *collect_requires;
static int collect_calls;
static unsigned tb_ring_collect(struct tb_ring *r,unsigned budget) {
    unsigned n=0;
    assert(r->flags&RING_FLAG_NO_INTERRUPT);
    if(collect_requires)assert(pthread_mutex_trylock(&collect_requires->m));
    ++collect_calls;++r->polls;
    if(!r->running)return 0;
    /* As tb_ring_collect(): one set of stamps for the whole batch. */
    if(!list_empty(&r->done)){r->cb_stamps.irq_ns=0;r->cb_stamps.start_ns=ktime_get_ns();r->cb_stamps.reap_ns=ktime_get_ns();}
    while(n<budget&&!list_empty(&r->done)) {
        struct ring_frame *f=container_of(r->done.next,struct ring_frame,list);
        list_del_init(&f->list);f->callback(r,f,false);++n;
    }
    if(n)++r->collections;
    return n;
}
/* The ring model for the stall detector: r->queue is posted and not
 * finished, r->done finished and not collected (the front). A test may
 * require a lock (ctx->lock) not to be held by the caller. */
struct tb_ring_snapshot { u32 head, tail, queued; bool front_completed, running; };
static spinlock_t *snapshot_forbids;
static int snapshot_calls;
static void (*snapshot_hook)(struct tb_ring *); /* runs between the read and its store */
static void tb_ring_snapshot(struct tb_ring *r,struct tb_ring_snapshot *s) {
    struct list_head *p;u32 posted=0,done=0;
    if(snapshot_forbids){assert(!pthread_mutex_trylock(&snapshot_forbids->m));pthread_mutex_unlock(&snapshot_forbids->m);}
    for(p=r->queue.next;p!=&r->queue;p=p->next)++posted;
    for(p=r->done.next;p!=&r->done;p=p->next)++done;
    s->head=posted+done;s->tail=0;s->queued=0;s->front_completed=done>0;s->running=r->running;
    ++snapshot_calls;
    if(snapshot_hook)snapshot_hook(r);
}
static bool flush_work(struct work_struct *work) {
    struct tb_ring *ring = container_of(work, struct tb_ring, work);
    if (!ring->running && !stop_fails)
        tb_ring_stop(ring);
    return true;
}
static void tb_ring_free(struct tb_ring *r) { assert(!r->running&&list_empty(&r->queue)&&list_empty(&r->done));pthread_mutex_destroy(&r->lock.m);free(r); }
static int tb_ring_throttling(struct tb_ring *r,unsigned interval) { (void)r;(void)interval;return 0; }
static int enqueue_fail;
static int __tb_ring_enqueue_list(struct tb_ring *r,struct list_head *frames) {
    if(enqueue_fail){enqueue_fail=0;return -ESHUTDOWN;}
    if(!r->running)return -ESHUTDOWN;
    while(!list_empty(frames)) { struct list_head *p=frames->next;list_del_init(p);list_add_tail(p,&r->queue); }
    return 0;
}
struct tb_cm_ops { void *driver_ready; };
struct tb { struct tb_nhi *nhi;struct tb_cm_ops *cm_ops;struct device dev; };
static unsigned retained_reports;
static void tb_domain_dma_retained(struct tb *tb) { (void)tb; ++retained_reports; }
struct tb_xdomain { struct tb *tb;uuid_t *local_uuid,*remote_uuid;struct device dev;u64 route;bool is_unplugged; };
struct tb_service { struct device dev;struct tb_xdomain *xd; };
struct tbstream { struct tb_service *svc; };
struct tbstream_gpu_context;
struct tbstream_dev { struct mutex lock;struct tbstream *stream;struct { struct tb_ring *ring; } tx_ring,rx_ring;struct tbstream_gpu_context *gpu_context;unsigned ring_size,throttling,in_hopid,out_hopid,rx_repost_batch,tx_submit_batch,rx_window_bytes;bool busy_poll,rx_busy_poll,rx_napi,removed,gpu_direct;u64 gpu_last_epoch;int wait,refs,users;int cfg_gpu_poll;int cfg_latency_qos_us; };
/* CPU latency QoS: the core's request API, and stream.c's helpers verbatim. */
struct pm_qos_request { bool active; int value; };
static int qos_live, qos_adds, latency_qos_us = -1;
#define TBSTREAM_DEV_MAX_QOS_US 100000
static bool cpu_latency_qos_request_active(struct pm_qos_request *r) { return r->active; }
static void cpu_latency_qos_add_request(struct pm_qos_request *r, int v) { assert(!r->active); r->active = true; r->value = v; ++qos_live; ++qos_adds; }
static void cpu_latency_qos_update_request(struct pm_qos_request *r, int v) { assert(r->active); r->value = v; }
static void cpu_latency_qos_remove_request(struct pm_qos_request *r) { assert(r->active); memset(r, 0, sizeof(*r)); --qos_live; }
static int tbstream_qos_acquire(struct tbstream_dev *sdev, struct pm_qos_request *req)
{
	int us = sdev->cfg_latency_qos_us < 0 ? READ_ONCE(latency_qos_us) :
		 sdev->cfg_latency_qos_us;

	if (us < 0)
		return -1;
	us = min(us, TBSTREAM_DEV_MAX_QOS_US);
	if (cpu_latency_qos_request_active(req))
		cpu_latency_qos_update_request(req, us);
	else
		cpu_latency_qos_add_request(req, us);
	return us;
}
static void tbstream_qos_release(struct pm_qos_request *req)
{
	if (cpu_latency_qos_request_active(req))
		cpu_latency_qos_remove_request(req);
}
static struct tb_xdomain *tbstream_dev_xdomain(struct tbstream_dev *s) { return s->stream?s->stream->svc->xd:NULL; }
static struct tbstream *tbstream_get(struct tbstream *s) { return s; }
static void tbstream_put(struct tbstream *s) { (void)s; }
static void tbstream_dev_get(struct tbstream_dev *s) { ++s->refs; }
static void tbstream_dev_put(struct tbstream_dev *s) { assert(s->refs > 0); --s->refs; }
static void tbstream_dev_irq_affinity(struct tbstream_dev *s) { (void)s; }
static int affinity_restores;
static void tbstream_dev_irq_affinity_restore(struct tbstream_dev *s) { (void)s; ++affinity_restores; }
static int tbstream_dev_valid(struct tbstream_dev *s) { return (!s->stream||s->removed)?-ENODEV:0; }
static struct tbstream_dev *to_tbstream_dev(void *p) { return p; }
static int reset_error, reset_calls;
static int tb_domain_reset_dma(struct tb *tb, struct tb_ring *tx, struct tb_ring *rx) {
    (void)tb; assert(!signalling_depth);
    assert(!tx || (!tx->running && list_empty(&tx->queue)));
    assert(!rx || (!rx->running && list_empty(&rx->queue)));
    ++reset_calls; return reset_error;
}
static int domain_paths_busy, path_queries, released_in_hopid, released_out_hopid, disable_calls;
static struct tb *last_path_domain;
static struct tb_xdomain *last_disabled_xd, *last_hopid_xd;
static int last_disable_tx, last_disable_rx;
static int tb_domain_dma_paths_busy(struct tb *tb, int tx, int rx) {
    assert(tx > 0 && rx > 0); ++path_queries; last_path_domain=tb; return domain_paths_busy;
}
static void tb_xdomain_release_in_hopid(struct tb_xdomain *xd, int hopid) { last_hopid_xd=xd;released_in_hopid=hopid; }
static void tb_xdomain_release_out_hopid(struct tb_xdomain *xd, int hopid) { last_hopid_xd=xd;released_out_hopid=hopid; }
static int enable_fail,disable_fail;
static int tb_xdomain_enable_paths(struct tb_xdomain *x,int a,int b,int c,int d) { (void)x;(void)a;(void)b;(void)c;(void)d;return enable_fail?-EIO:0; }
static int tb_xdomain_disable_paths(struct tb_xdomain *x,int a,int b,int c,int d) { assert(!x->tb->nhi->dma_shutdown);
    (void)b;(void)d;++disable_calls;last_disabled_xd=x;last_disable_tx=a;last_disable_rx=c;return disable_fail?-EIO:0;
}
static void poll_wait(struct file *f,int *w,poll_table *p) { (void)f;(void)w;(void)p; }

static unsigned group_reset_calls, group_reset_count;
static struct tb_ring *group_reset_rings[32];
static int tb_domain_reset_dma_group(struct tb *tb, struct tb_ring *const *rings, unsigned count, unsigned flags, bool *performed) {
    (void)tb; assert(flags <= 1); *performed = (reset_error == 0 || reset_error == -EIO); assert(count > 0 && count <= 32); ++group_reset_calls; group_reset_count=count;
    for (unsigned i=0;i<count;++i) { assert(rings[i] && !rings[i]->running); group_reset_rings[i]=rings[i]; }
    return reset_error;
}
#endif
