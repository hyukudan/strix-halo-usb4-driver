/* SPDX-License-Identifier: GPL-2.0-only */
/* API/MMIO doubles, not an electrical/NHI simulator. */
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
typedef uint32_t u32;
typedef uint16_t u16;
#define TB_DMA_RESET_MAX_RINGS 32U
#define TB_DMA_DOMAIN_QUIESCING 1UL
#define TB_DMA_GROUP_RECOVERY 2UL
#define TB_DMA_DOMAIN_DMA_REVOKED 3UL
#define TB_DMA_DOMAIN_ABANDONED 4UL
#define TB_DMA_RESET_WITH_NETWORK 1U
struct notifier_block { int unused; };
struct blocking_notifier_head { int unused; };
#define BLOCKING_NOTIFIER_HEAD(name) struct blocking_notifier_head name = {0}
static int blocking_notifier_chain_register(struct blocking_notifier_head *h, struct notifier_block *n) { (void)h;(void)n;return 0; }
static int blocking_notifier_chain_unregister(struct blocking_notifier_head *h, struct notifier_block *n) { (void)h;(void)n;return 0; }
static unsigned core_notify_calls;
static unsigned long core_events[8];
struct tb;
static void (*teardown_consumer)(unsigned long event, struct tb *tb);
static int blocking_notifier_call_chain(struct blocking_notifier_head *h,unsigned long event, void *data) {
    (void)h;assert(event>=TB_DMA_DOMAIN_QUIESCING && event<=TB_DMA_DOMAIN_ABANDONED);assert(data);
    assert(core_notify_calls<8);core_events[core_notify_calls++]=event;
    if (teardown_consumer && event!=TB_DMA_GROUP_RECOVERY) teardown_consumer(event,data);
    return 0;
}
#define BIT(n) (1U << (n))
#define FIELD_GET(mask,v) (((v) & (mask)) >> __builtin_ctz(mask))
#define REG_CAPS 0x39640
#define REG_CAPS_VERSION_MASK 0x00ff0000U
#define REG_CAPS_VERSION_2 0x40
#define PCI_VENDOR_ID_AMD 0x1022
#define PCI_DEVICE_ID_AMD_1AH_M60H_NHI0 1
#define PCI_DEVICE_ID_AMD_1AH_M60H_NHI1 2
#define PCI_DEVICE_ID_AMD_1AH_M68H_NHI0 3
#define PCI_DEVICE_ID_AMD_1AH_M68H_NHI1 4
#define PCI_DEVICE_ID_AMD_1AH_M80H_NHI0 5
#define PCI_DEVICE_ID_AMD_1AH_M80H_NHI1 6
#define PCI_DEVICE_ID_AMD_1AH_M80H_NHI2 7
#define PCI_DEVICE_ID_AMD_1AH_M24H_NHI0 8
#define PCI_DEVICE_ID_AMD_1AH_M24H_NHI1 9
#define PCI_DEVICE_ID_AMD_1AH_M70H_NHI0 10
#define PCI_DEVICE_ID_AMD_1AH_M70H_NHI1 11
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x,v) ((x)=(v))
/* The last message of each level, so cases can check what dmesg would say. */
static char last_info[256], last_warn[256], last_err[256];
static int infos, warns, errs;
#define dev_info(d, ...) ((void)(d), ++infos, snprintf(last_info, sizeof(last_info), __VA_ARGS__))
#define dev_warn(d, ...) ((void)(d), ++warns, snprintf(last_warn, sizeof(last_warn), __VA_ARGS__))
#define dev_err(d, ...) ((void)(d), ++errs, snprintf(last_err, sizeof(last_err), __VA_ARGS__))
#define dev_dbg(d, ...) ((void)(d))
#define module_param(name, type, perm)
#define MODULE_PARM_DESC(name, desc)
#define container_of(p, t, m) ((t *)((char *)(p) - offsetof(t, m)))
/* Delayed work: queued with a delay, run explicitly by the cases. */
struct work_struct { int unused; };
struct workqueue_struct { int unused; };
struct delayed_work { struct work_struct work; void (*func)(struct work_struct *); bool pending; unsigned delay_ms; struct workqueue_struct *wq; };
static unsigned queued_delays[32], nqueued;
#define msecs_to_jiffies(ms) (ms)
static struct delayed_work *to_delayed_work(struct work_struct *w) { return container_of(w, struct delayed_work, work); }
static bool queue_delayed_work(struct workqueue_struct *wq, struct delayed_work *w, unsigned delay)
{
    if (w->pending) return false;
    w->pending = true; w->delay_ms = delay; w->wq = wq;
    assert(nqueued < 32); queued_delays[nqueued++] = delay;
    return true;
}
static bool mod_delayed_work(struct workqueue_struct *wq, struct delayed_work *w, unsigned delay)
{
    bool was = w->pending;
    w->pending = false;
    queue_delayed_work(wq, w, delay);
    return was;
}
/* Runs the work once if it is queued; returns whether it ran. */
static bool run_delayed(struct delayed_work *w)
{
    if (!w->pending) return false;
    w->pending = false;
    w->func(&w->work);
    return true;
}
#define PCI_COMMAND 0x04
#define PCI_COMMAND_MASTER 0x4
#define EXPORT_SYMBOL_GPL(x)
struct mutex { pthread_mutex_t value; };
typedef struct mutex spinlock_t;
static void mutex_init(struct mutex *m) { assert(!pthread_mutex_init(&m->value, NULL)); }
static void mutex_lock(struct mutex *m) { assert(!pthread_mutex_lock(&m->value)); }
static bool mutex_trylock(struct mutex *m) { return !pthread_mutex_trylock(&m->value); }
static void mutex_unlock(struct mutex *m) { assert(!pthread_mutex_unlock(&m->value)); }
#define spin_lock mutex_lock
#define spin_unlock mutex_unlock
#define spin_lock_irqsave(p,f) do { (f)=0; spin_lock(p); } while (0)
#define spin_unlock_irqrestore(p,f) do { (void)(f); spin_unlock(p); } while (0)
struct list_head { struct list_head *next, *prev; };
static void INIT_LIST_HEAD(struct list_head *l) { l->next=l->prev=l; }
static bool list_empty(const struct list_head *l) { return l->next==l; }
struct pci_dev;
struct device { bool registered, pci; struct pci_dev *pdev; };
struct pci_dev { unsigned vendor, device; bool present, master, master_stuck, config_fail, relaxed, devctl_fail; struct device dev; };
struct tb_nhi;
struct tb_ring { spinlock_t lock; struct tb_nhi *nhi; bool is_tx, running; unsigned hop; struct list_head queue, in_flight; };
struct tb_nhi { spinlock_t lock; struct device *dev; struct tb_ring *tx_rings[8], *rx_rings[8]; bool going_away, dma_resetting, dma_shutdown; unsigned dma_retained; bool dma_revoked, dma_abandoned; unsigned hop_count; unsigned char *iobase; };
struct tb_ctl { struct tb_nhi *nhi; };
struct tb;
struct tb_cm_ops { int (*dma_paths_busy)(struct tb *, int, int); };
struct tb { struct tb_cm_ops *cm_ops; struct tb_nhi *nhi; struct mutex lock; struct device dev; struct tb_ctl *ctl;
            struct workqueue_struct *wq; struct delayed_work teardown_reset_work; unsigned int teardown_reset_attempts; };
static bool dev_is_pci(struct device *d) { return d->pci; }
static struct pci_dev *to_pci_dev(struct device *d) { return d->pdev; }
static bool device_is_registered(struct device *d) { return d->registered; }
static bool pci_device_is_present(struct pci_dev *p) { return p->present; }
static void pci_clear_master(struct pci_dev *p) { if (!p->master_stuck) p->master=false; }
#define PCI_EXP_DEVCTL 8
#define PCI_EXP_DEVCTL_RELAX_EN 0x0010
static int pcie_capability_read_word(struct pci_dev *p, int where, uint16_t *value) {
    assert(where == PCI_EXP_DEVCTL);
    if (p->devctl_fail) return -EIO;
    *value = p->relaxed ? PCI_EXP_DEVCTL_RELAX_EN : 0;
    return 0;
}
static int pci_read_config_word(struct pci_dev *p, int where, uint16_t *value) {
    assert(where==PCI_COMMAND); if (p->config_fail) return -EIO; *value=p->master?PCI_COMMAND_MASTER:0; return 0;
}
static int pm_refs, pm_error, writes, reads, starts, stops;
static bool broken_readback, broken_control;
static unsigned char bar[0x40000];
static u32 caps;
static int pm_runtime_resume_and_get(struct device *d) { (void)d; if(pm_error)return pm_error; ++pm_refs;return 0; }
static void pm_runtime_mark_last_busy(struct device *d) { (void)d; }
static void pm_runtime_put_autosuspend(struct device *d) { (void)d;assert(pm_refs>0);--pm_refs; }
static u32 ioread32(void *address) { assert((unsigned char *)address==bar+REG_CAPS); ++reads;return writes&&broken_readback?~0U:caps; }
static void iowrite32(u32 value,void *address) { assert(value==1 && (unsigned char *)address==bar+0x39858);++writes; }
static void usleep_range(unsigned a,unsigned b) { assert(a==10000&&b==20000); }
static void tb_ctl_stop(struct tb_ctl *ctl) {
    ++stops;ctl->nhi->tx_rings[0]->running=false;ctl->nhi->rx_rings[0]->running=false;
}
static void tb_ctl_start(struct tb_ctl *ctl) {
    ++starts;if(broken_control)return;
    ctl->nhi->tx_rings[0]->running=true;ctl->nhi->rx_rings[0]->running=true;
}

static int inventory_result, inventory_calls;
static int fake_inventory(struct tb *tb, int tx, int rx) { (void)tb; assert(tx == -1 || tx > 0); assert(rx == -1 || rx > 0); ++inventory_calls;return inventory_result; }
static struct tb_cm_ops fake_cm_ops = { .dma_paths_busy = fake_inventory };

struct tb_dma_reset_request {
    struct tb *tb; struct tb_ring *const *rings; unsigned nrings;
    bool *performed; bool handled; int status;
    int (*execute)(struct tb_dma_reset_request *request);
};
