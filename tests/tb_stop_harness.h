/* SPDX-License-Identifier: GPL-2.0-only */
/* Doubles for running the actual tb_stop() of the patched tb.c, followed by
 * the actual DMA inventory query, under AddressSanitizer. Tunnels are heap
 * objects freed by tb_tunnel_put(), exactly like the real reference drop, so
 * a query that walks a freed tunnel is reported as heap-use-after-free. */
#ifndef TB_STOP_HARNESS_H
#define TB_STOP_HARNESS_H
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define container_of(p, t, m) ((t *)((char *)(p) - offsetof(t, m)))
struct list_head { struct list_head *next, *prev; };
static void INIT_LIST_HEAD(struct list_head *l) { l->next = l->prev = l; }
static bool list_empty(const struct list_head *l) { return l->next == l; }
static void list_add_tail(struct list_head *n, struct list_head *h)
{
    n->prev = h->prev; n->next = h; h->prev->next = n; h->prev = n;
}
static void list_del(struct list_head *n)
{
    n->prev->next = n->next; n->next->prev = n->prev;
    n->next = (void *)0x100; n->prev = (void *)0x122; /* LIST_POISON */
}
#define list_entry(p, t, m) container_of(p, t, m)
#define list_for_each_entry(pos, head, member) \
    for (pos = list_entry((head)->next, __typeof__(*pos), member); \
         &pos->member != (head); \
         pos = list_entry(pos->member.next, __typeof__(*pos), member))
#define list_for_each_entry_safe(pos, n, head, member) \
    for (pos = list_entry((head)->next, __typeof__(*pos), member), \
         n = list_entry(pos->member.next, __typeof__(*pos), member); \
         &pos->member != (head); \
         pos = n, n = list_entry(n->member.next, __typeof__(*n), member))

struct delayed_work { int pending; };
static void cancel_delayed_work(struct delayed_work *w) { w->pending = 0; }

struct tb_switch;
struct tb_port { struct tb_switch *sw; struct tb_port *remote; bool null_port; };
struct tb_switch { int ports; struct tb_port port[2]; bool usb4; bool removed; };
struct tb_nhi { bool host_reset; };
struct tb_tunnel {
    struct list_head list;
    bool dma, active;
    int tx_ring, rx_ring;
};
struct tb_cm {
    struct list_head tunnel_list;
    struct delayed_work remove_work;
    bool hotplug_active;
};
struct tb { int lock; struct tb_nhi *nhi; struct tb_switch *root_switch; struct tb_cm cm; };

static struct tb_cm *tb_priv(struct tb *tb) { return &tb->cm; }
#define lockdep_assert_held(l) assert(*(l))
static bool tb_tunnel_is_dma(const struct tb_tunnel *t) { return t->dma; }
static int deactivated, puts_done;
static void tb_tunnel_deactivate(struct tb_tunnel *t) { assert(t->active); t->active = false; ++deactivated; }
static void tb_tunnel_put(struct tb_tunnel *t) { ++puts_done; free(t); }
static bool tb_tunnel_match_dma(const struct tb_tunnel *t, int tx_path, int tx_ring,
                                int rx_path, int rx_ring)
{
    (void)tx_path; (void)rx_path;
    return (tx_ring <= 0 || t->tx_ring == tx_ring) && (rx_ring <= 0 || t->rx_ring == rx_ring);
}
#define tb_switch_for_each_port(sw, p) \
    for ((p) = &(sw)->port[0]; (p) < &(sw)->port[(sw)->ports]; (p)++)
static bool tb_port_is_null(const struct tb_port *p) { return p->null_port; }
static bool tb_port_has_remote(const struct tb_port *p) { return p->remote; }
static bool tb_switch_is_usb4(const struct tb_switch *sw) { return sw->usb4; }
static int tb_port_reset(struct tb_port *p) { (void)p; return 0; }
#define tb_port_dbg(p, ...) ((void)(p))
static void tb_switch_remove(struct tb_switch *sw) { sw->removed = true; }
#endif
