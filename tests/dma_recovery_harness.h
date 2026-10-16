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
#define EXPORT_SYMBOL_GPL(x)
struct mutex { pthread_mutex_t value; };
typedef struct mutex spinlock_t;
static void mutex_init(struct mutex *m) { assert(!pthread_mutex_init(&m->value, NULL)); }
static void mutex_lock(struct mutex *m) { assert(!pthread_mutex_lock(&m->value)); }
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
struct pci_dev { unsigned vendor, device; bool present; struct device dev; };
struct tb_nhi;
struct tb_ring { spinlock_t lock; struct tb_nhi *nhi; bool is_tx, running; unsigned hop; struct list_head queue, in_flight; };
struct tb_nhi { spinlock_t lock; struct device *dev; struct tb_ring *tx_rings[8], *rx_rings[8]; bool going_away, dma_resetting, dma_shutdown; unsigned hop_count; unsigned char *iobase; };
struct tb_ctl { struct tb_nhi *nhi; };
struct tb { struct tb_nhi *nhi; struct mutex lock; struct device dev; struct tb_ctl *ctl; };
static bool dev_is_pci(struct device *d) { return d->pci; }
static struct pci_dev *to_pci_dev(struct device *d) { return d->pdev; }
static bool device_is_registered(struct device *d) { return d->registered; }
static bool pci_device_is_present(struct pci_dev *p) { return p->present; }
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
