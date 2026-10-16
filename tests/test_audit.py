#!/usr/bin/env python3
"""Hardware-free tests of actual patched C functions plus sysfs parsing.

The C shims model lists/registers, not DMA timing, cache coherency or hardware.
Use USB4_SOURCE_TREE for an already materialized overlay.
"""
import importlib.util
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[1]
SOURCE = Path(os.environ.get('USB4_SOURCE_TREE', REPO / 'audited-overlay'))

def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module

def function(path, name):
    source = (SOURCE / path).read_text()
    match = re.search(r'\b' + re.escape(name) + r'\([^;{}]*\)\s*\{', source)
    if not match:
        raise AssertionError(f'{name} not found in {path}')
    start = source.rfind('\n', 0, match.start()) + 1
    depth = 1
    end = match.end()
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end] + '\n'

def compile_run(code):
    with tempfile.TemporaryDirectory() as tmp:
        src = Path(tmp) / 'test.c'
        exe = Path(tmp) / 'test'
        src.write_text(code)
        subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-Wall', '-Wextra',
                        '-Werror', '-Wno-unused-function', '-Wno-unused-parameter',
                        '-fsanitize=undefined', '-fno-sanitize-recover=all',
                        '-I' + str(SOURCE / 'include/uapi'),
                        '-I' + str(SOURCE / 'drivers/thunderbolt'),
                        str(src), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)

COMMON = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <limits.h>
#include <string.h>
typedef uint32_t u32;
typedef uint16_t u16;
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x, v) ((x) = (v))
#define min(a,b) ((a) < (b) ? (a) : (b))
#define min_not_zero(a,b) (!(a) ? (b) : !(b) ? (a) : min(a,b))
#define spin_lock_irqsave(p,f) do { (void)(p); (f)=0; } while (0)
#define spin_unlock_irqrestore(p,f) do { (void)(p); (void)(f); } while (0)
'''
LISTS = r'''
struct list_head { struct list_head *next, *prev; };
#define LIST_HEAD(n) struct list_head n = { &(n), &(n) }
#define INIT_LIST_HEAD(h) do { (h)->next=(h); (h)->prev=(h); } while (0)
#define container_of(p,t,m) ((t *)((char *)(p)-offsetof(t,m)))
static bool list_empty(struct list_head *h) { return h->next == h; }
static void list_add_tail(struct list_head *n, struct list_head *h) {
    n->prev=h->prev; n->next=h; h->prev->next=n; h->prev=n;
}
static void list_del_init(struct list_head *n) {
    n->prev->next=n->next; n->next->prev=n->prev; INIT_LIST_HEAD(n);
}
static void list_move_tail(struct list_head *n, struct list_head *h) {
    list_del_init(n); list_add_tail(n,h);
}
static void list_splice_tail_init(struct list_head *a, struct list_head *b) {
    if (list_empty(a)) return;
    a->next->prev=b->prev; b->prev->next=a->next;
    a->prev->next=b; b->prev=a->prev; INIT_LIST_HEAD(a);
}
#define list_for_each_entry_safe(pos,n,h,m) \
    for (struct list_head *_p=(h)->next, *_next; \
         _p != (h) && ((_next=_p->next), \
         (pos)=container_of(_p,__typeof__(*(pos)),m), \
         (n)=_next != (h) ? container_of(_next,__typeof__(*(n)),m) : NULL, \
         (void)(n), 1); _p=_next)
'''

class AuditTests(unittest.TestCase):
    def test_credit_limits_and_shared_rx_budget(self):
        prelude = r'''
#define TB_MIN_DMA_CREDITS 1
#define TB_MAX_CREDITS 127
#define TB_DMA_PRIORITY 5
#define TB_DMA_WEIGHT 1
#define TB_PATH_NONE 0
#define TB_PATH_SOURCE 1
#define TB_PATH_INTERNAL 2
#define TB_PATH_ALL 3
struct kernel_param { void *arg; };
struct tb_switch { unsigned int max_usb3_credits, max_pcie_credits, max_dma_credits;
    unsigned int min_dp_aux_credits, min_dp_main_credits; };
struct tb_port { unsigned int total_credits, ctl_credits, dma_credits;
    struct tb_switch *sw; bool allocation; };
struct tb_path_hop { struct tb_port *in_port; unsigned int initial_credits; };
struct tb_path { struct tb_path_hop hops[2]; unsigned int path_length;
    int egress_fc_enable, ingress_fc_enable, egress_shared_buffer, ingress_shared_buffer;
    int priority, weight; bool clear_fc; };
static unsigned int dma_credits=14;
static int kstrtouint(const char *s, unsigned int base, unsigned int *v) {
    char *end; unsigned long value;
    errno=0; if (*s=='-') return -EINVAL;
    value=strtoul(s,&end,base);
    if (*end || end==s) return -EINVAL;
    if (errno || value>UINT_MAX) return -ERANGE;
    *v=value; return 0;
}
#define tb_acpi_may_tunnel_usb3() true
#define tb_acpi_may_tunnel_pcie() true
#define tb_acpi_is_xdomain_allowed() true
#define tb_acpi_may_tunnel_dp() true
#define tb_port_dbg(...) do {} while (0)
static bool tb_port_use_credit_allocation(const struct tb_port *p) { return p->allocation; }
static int tb_dma_reserve_credits(struct tb_path_hop *h, unsigned int c) { return 0; }
'''
        code = COMMON + prelude
        for name in ('tb_dma_credits_set', 'tb_usable_credits', 'tb_available_credits',
                     'tb_dma_init_rx_path', 'tb_dma_release_credits'):
            code += function('drivers/thunderbolt/tunnel.c', name)
        code += r'''
int main(void) {
    struct kernel_param kp={.arg=&dma_credits};
    struct tb_switch sw={.max_usb3_credits=8,.max_pcie_credits=18,
        .min_dp_aux_credits=1,.min_dp_main_credits=16};
    struct tb_port p={.total_credits=60,.ctl_credits=4,.sw=&sw,.allocation=true};
    struct tb_path a={.path_length=1}, b={.path_length=1}, c={.path_length=1};
    const char *bad[]={"0","128","4294967295","-1","nonsense"};
    size_t ndp;
    for (size_t i=0;i<sizeof(bad)/sizeof(*bad);i++) {
        unsigned int old=dma_credits;
        assert(tb_dma_credits_set(bad[i],&kp)<0); assert(dma_credits==old);
    }
    assert(!tb_dma_credits_set("1",&kp)); assert(dma_credits==1);
    assert(!tb_dma_credits_set("127",&kp)); assert(dma_credits==127);
    a.hops[0].in_port=b.hops[0].in_port=c.hops[0].in_port=&p;
    assert(!tb_dma_init_rx_path(&a,32)); assert(a.hops[0].initial_credits==32);
    assert(!tb_dma_init_rx_path(&b,32)); assert(b.hops[0].initial_credits==24);
    assert(p.dma_credits==56);
    assert(tb_dma_init_rx_path(&c,32)==-ENOSPC); assert(p.dma_credits==56);
    tb_dma_release_credits(&b.hops[0]); tb_dma_release_credits(&a.hops[0]);
    assert(p.dma_credits==0);
    p.allocation=false;
    assert(!tb_dma_init_rx_path(&a,14)); assert(p.dma_credits==0);
    p.total_credits=2; p.ctl_credits=4;
    assert(tb_usable_credits(&p)==0); assert(tb_dma_init_rx_path(&c,14)==-ENOSPC);
    p.total_credits=16; p.ctl_credits=2; sw.max_usb3_credits=16;
    assert(tb_available_credits(&p,&ndp)==0); assert(ndp==0);
    p.total_credits=120; sw.max_usb3_credits=8; dma_credits=14;
    assert(tb_available_credits(&p,&ndp)==42); assert(ndp==4);
    puts("credit bounds / shared RX exhaustion / release / legacy / underflow: PASS");
}
'''
        compile_run(code)

    def test_batched_ring_and_interrupt_banks(self):
        code = COMMON + LISTS + r'''
#define BIT(n) (1U << (n))
#define RING_DESC_POSTED 1
#define RING_DESC_INTERRUPT 2
#define RING_FLAG_NO_INTERRUPT 1
#define QUIRK_AUTO_CLEAR_INT 1
#define REG_RING_INT_CLEAR 0x100
#define REG_RING_INTERRUPT_BASE 0x200
struct ring_frame { struct list_head list; uint64_t buffer_phy; u16 size, sof, eof; };
struct ring_desc { uint64_t phys; u32 time,flags; u16 length,sof,eof; };
struct tb_nhi { char *iobase; unsigned int hop_count,quirks; };
struct tb_ring { int lock,hop; bool running,is_tx; unsigned int flags;
    u16 head,tail,size; struct ring_desc *descriptors;
    struct list_head queue,in_flight; struct tb_nhi *nhi; };
static unsigned int doorbells, barriers;
static uintptr_t write_addr, read_addr;
static u32 write_value;
#define dma_wmb() (++barriers)
static void ring_iowrite_prod(struct tb_ring *r,u16 h) { assert(barriers); assert(r->head==h); doorbells++; }
static void ring_iowrite_cons(struct tb_ring *r,u16 h) { ring_iowrite_prod(r,h); }
static int ring_interrupt_index(const struct tb_ring *r) { return r->hop+(r->is_tx ? 0 : r->nhi->hop_count); }
static void iowrite32(u32 v,void *p) { write_addr=(uintptr_t)p; write_value=v; }
static u32 ioread32(void *p) { read_addr=(uintptr_t)p; return 0; }
'''
        for name in ('ring_full', 'ring_write_descriptors', '__tb_ring_enqueue',
                     '__tb_ring_enqueue_list', 'ring_clear_msix'):
            code += function('drivers/thunderbolt/nhi.c', name)
        code += r'''
static unsigned int count(struct list_head *h) {
    unsigned int n=0; for (struct list_head *p=h->next;p!=h;p=p->next) n++; return n;
}
static void init(struct tb_ring *r,struct ring_desc *d,u16 size,bool tx) {
    memset(r,0,sizeof(*r)); memset(d,0,size*sizeof(*d));
    r->running=true; r->is_tx=tx; r->size=size; r->descriptors=d;
    INIT_LIST_HEAD(&r->queue); INIT_LIST_HEAD(&r->in_flight); doorbells=barriers=0;
}
int main(void) {
    struct tb_ring r;
    struct ring_desc d[513];
    struct ring_frame f[512];
    char registers[1024];
    struct tb_nhi nhi={.iobase=registers};
    LIST_HEAD(batch);
    for (int tx=0;tx<2;tx++) {
        init(&r,d,4,tx);
        assert(!__tb_ring_enqueue_list(&r,&batch)); assert(!doorbells);
        for (int i=0;i<4;i++) {
            memset(&f[i],0,sizeof(f[i])); f[i].buffer_phy=100+i; f[i].size=i+1;
            list_add_tail(&f[i].list,&batch);
        }
        assert(!__tb_ring_enqueue_list(&r,&batch)); assert(list_empty(&batch));
        assert(doorbells==1 && r.head==3 && count(&r.in_flight)==3 && count(&r.queue)==1);
        for (int i=0;i<3;i++) { assert(d[i].phys==100U+i); assert(d[i].flags==3); }
        ring_write_descriptors(&r); assert(doorbells==1);
        list_del_init(r.in_flight.next); r.tail=1;
        ring_write_descriptors(&r);
        assert(doorbells==2 && r.head==0 && list_empty(&r.queue));
        while (!list_empty(&r.in_flight)) list_del_init(r.in_flight.next);
        init(&r,d,4,tx); r.running=false;
        list_add_tail(&f[0].list,&batch); list_add_tail(&f[1].list,&batch);
        assert(__tb_ring_enqueue_list(&r,&batch)==-ESHUTDOWN);
        assert(count(&batch)==2 && !doorbells && list_empty(&r.in_flight));
        while (!list_empty(&batch)) list_del_init(batch.next);
    }
    init(&r,d,513,true);
    for (int i=0;i<512;i++) { INIT_LIST_HEAD(&f[i].list); list_add_tail(&f[i].list,&batch); }
    assert(!__tb_ring_enqueue_list(&r,&batch)); assert(doorbells==1);
    while (!list_empty(&r.in_flight)) list_del_init(r.in_flight.next);
    init(&r,d,513,true);
    for (int i=0;i<512;i++) assert(!__tb_ring_enqueue(&r,&f[i]));
    assert(doorbells==512);
    for (unsigned int hops=12;hops<=64;hops+=1) {
        nhi.hop_count=hops; r.nhi=&nhi;
        for (unsigned int hop=0;hop<hops;hop++) for (int tx=0;tx<2;tx++) {
            unsigned int index=hop+(tx ? 0 : hops);
            r.hop=hop; r.is_tx=tx; nhi.quirks=0;
            ring_clear_msix(&r);
            assert(write_addr==(uintptr_t)registers+REG_RING_INT_CLEAR+4*(index/32));
            assert(read_addr==(uintptr_t)registers+REG_RING_INTERRUPT_BASE+4*(index/32));
            assert(write_value==BIT(index%32));
        }
    }
    write_addr=read_addr=0; nhi.quirks=QUIRK_AUTO_CLEAR_INT; ring_clear_msix(&r);
    assert(!write_addr && !read_addr);
    puts("ring empty/full/wrap/stopped/TX/RX; 512 vs 1 doorbells; interrupt banks: PASS");
}
'''
        compile_run(code)

    def test_real_shared_header(self):
        compile_run(COMMON + r'''
#include "gpu_stream_logic.h"
int main(void) {
    struct sgu_controller_lifetime l={0};
    struct sgu_prepare p={.handle=1,.sequence=1,.payload_bytes=1,.terminal_bytes=1};
    struct sgu_dma_segment s[2]={{0,0,4096},{4096,8192,8192}};
    __u32 n=0,cursor=0; __u64 dma=999;
    assert(!sgu_controller_queue_batch(&l,0));
    assert(!sgu_controller_queue_batch(&l,UINT_MAX));
    assert(sgu_controller_queue_batch(&l,SGU_MAX_FRAMES));
    assert(sgu_controller_queue_batch(&l,SGU_MAX_FRAMES));
    assert(!sgu_controller_queue(&l)); assert(!sgu_controller_stopped(&l));
    for (unsigned int i=0;i<2U*SGU_MAX_FRAMES;i++) assert(sgu_controller_callback(&l));
    assert(!sgu_controller_callback(&l)); assert(sgu_controller_stopped(&l));
    assert(!sgu_controller_can_release(&l)); l.paths_removed=1;
    assert(sgu_controller_can_release(&l)); assert(!sgu_controller_queue(&l));
    assert(sgu_window_valid(32*4096,0,32*4096,32));
    assert(!sgu_window_valid(32*4096,1,32*4096,32));
    assert(!sgu_window_valid(31*4096,0,32*4096,32));
    assert(!sgu_prepare_valid(&p,0,&n)); assert(sgu_prepare_valid(&p,32,&n) && n==1);
    p.pool_offset=31*4096; assert(!sgu_prepare_valid(&p,32,&n));
    assert(sgu_rx_pdf_valid(256,0,2)); assert(!sgu_rx_pdf_valid(257,0,2));
    assert(sgu_rx_pdf_valid(4096,1,2)); assert(!sgu_rx_pdf_valid(4097,1,2));
    assert(sgu_resolve_frame(s,2,0,&cursor,&dma) && dma==0);
    assert(sgu_resolve_frame(s,2,4096,&cursor,&dma) && dma==8192);
    assert(!sgu_resolve_frame(s,2,10000,&cursor,&dma));
    puts("actual UAPI/shared helpers, lifetime boundaries, windows, PDFs and SG mapping: PASS");
}
''')

    def test_fence_and_stop_guards(self):
        free = function('drivers/thunderbolt/gpu_stream.c', 'sgu_fence_free')
        self.assertIn('dma_fence_free(fence)', free)
        self.assertNotIn('kfree(', free)
        stop = function('drivers/thunderbolt/gpu_stream.c', 'sgu_stop_dma_locked')
        self.assertIn('ctx->lifetime.stop_confirmed = 0', stop)
        self.assertIn('sgu_ring_running(sdev->rx_ring.ring)', stop)
        callback = function('drivers/thunderbolt/gpu_stream.c', 'sgu_callback')
        self.assertIn('if (wake)', callback)
        self.assertIn('op->event_pending', callback)
        enqueue = function('drivers/thunderbolt/gpu_stream.c', 'sgu_enqueue')
        self.assertLess(enqueue.index('sgu_controller_queue_batch'), enqueue.index('__tb_ring_enqueue_list'))
        self.assertIn('list_del_init', enqueue)
        self.assertIn('op->remaining = 0', enqueue)
        self.assertIn('flush_work', stop)
        wrapper = function('drivers/thunderbolt/gpu_stream.c', 'tbstream_gpu_stop')
        self.assertIn('sgu_stop_dma_locked(ctx)', wrapper)
        watchdog = function('drivers/thunderbolt/gpu_stream.c', 'sgu_watchdog')
        self.assertNotIn('sdev->lock', watchdog)
        finish = function('drivers/thunderbolt/gpu_stream.c', 'sgu_finish_signal')
        self.assertLess(finish.index('sgu_signal('), finish.index('op->event_pending = true'))

    def test_sysfs_per_lane_not_per_cable(self):
        diagnose = load('diagnose', REPO / 'tools/usb4_diagnose.py')
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp); device=root/'bus/thunderbolt/devices/0-1'
            device.mkdir(parents=True)
            for name,value in [('rx_speed','20.0 Gb/s'),('rx_lanes','2'),
                               ('tx_speed','20.0 Gb/s'),('tx_lanes','1')]:
                (device/name).write_text(value+'\n')
            link=diagnose.collect(root)['links'][0]
            self.assertEqual(link['rx']['raw_gbps'],40)
            self.assertEqual(link['tx']['raw_gbps'],20)
            (device/'rx_lanes').write_text('unknown')
            self.assertIsNone(diagnose.collect(root)['links'][0]['rx']['raw_gbps'])
            (device/'tx_speed').unlink()
            self.assertIsNone(diagnose.collect(root)['links'][0]['tx']['raw_gbps'])

if __name__ == '__main__':
    if not SOURCE.is_dir():
        raise SystemExit('Run tools/prepare_overlay.py audited-overlay first, or set USB4_SOURCE_TREE')
    unittest.main(verbosity=2)
