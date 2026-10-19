/* SPDX-License-Identifier: GPL-2.0 */
/* Inventory query using the matching semantics of pinned Linux tunnel.c,
 * 72d3fcf802c45d00b300f25b848a93c3a2bd7c7e, blob
 * 7e8284575dffb10b0d4da87786e2a7481024f673. Transport structs/list are doubles;
 * tb_tunnel_match_dma below is copied from that upstream implementation. */
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
struct tb_port { bool nhi; };
struct tb_path_hop { struct tb_port *in_port,*out_port;int in_hop_index,next_hop_index; };
struct tb_path { int path_length;struct tb_path_hop hops[2]; };
struct tb_tunnel { struct tb_tunnel *next;bool dma;int npaths;struct tb_path *paths[2]; };
struct tb_cm { struct tb_tunnel *tunnel_list; };
struct tb { bool lock;void *root_switch;struct tb_cm cm; };
static struct tb_cm *tb_priv(struct tb *tb) {return &tb->cm;}
static bool tb_tunnel_is_dma(const struct tb_tunnel *t) {return t->dma;}
static bool tb_port_is_nhi(const struct tb_port *p) {return p && p->nhi;}
#define lockdep_assert_held(l) assert(*(l))
#define list_for_each_entry(t,h,member) for((t)=*(h);(t);(t)=(t)->next)
#define list_empty(h) (!*(h))
static bool tb_tunnel_match_dma(const struct tb_tunnel *tunnel, int transmit_path,
                              int transmit_ring, int receive_path, int receive_ring)
{
    const struct tb_path *tx_path = NULL, *rx_path = NULL;
    int i;
    if (!receive_ring || !transmit_ring)
        return false;
    for (i = 0; i < tunnel->npaths; i++) {
        const struct tb_path *path = tunnel->paths[i];
        if (!path)
            continue;
        if (tb_port_is_nhi(path->hops[0].in_port))
            tx_path = path;
        else if (tb_port_is_nhi(path->hops[path->path_length - 1].out_port))
            rx_path = path;
    }
    if (transmit_ring > 0 || transmit_path > 0) {
        if (!tx_path)
            return false;
        if (transmit_ring > 0 && (tx_path->hops[0].in_hop_index != transmit_ring))
            return false;
        if (transmit_path > 0 &&
            (tx_path->hops[tx_path->path_length - 1].next_hop_index != transmit_path))
            return false;
    }
    if (receive_ring > 0 || receive_path > 0) {
        if (!rx_path)
            return false;
        if (receive_path > 0 && (rx_path->hops[0].in_hop_index != receive_path))
            return false;
        if (receive_ring > 0 &&
            (rx_path->hops[rx_path->path_length - 1].next_hop_index != receive_ring))
            return false;
    }
    return true;
}
#include "dma_path_query.h"
static void query_filters(void) {
    struct tb tb={.lock=true,.root_switch=&tb};
    struct tb_port nhi={true},port={false};
    struct tb_path tx={.path_length=2,.hops={{&nhi,&port,2,7},{&port,&port,7,9}}};
    struct tb_path rx={.path_length=2,.hops={{&port,&port,8,7},{&port,&nhi,7,3}}};
    struct tb_tunnel other={.dma=false,.npaths=2,.paths={&tx,&rx}};
    struct tb_tunnel tunnel={.dma=true,.npaths=2,.paths={&tx,&rx},.next=&other};
    assert(tb_dma_paths_busy(&tb,-1,-1)==0);
    tb.cm.tunnel_list=&other;
    assert(tb_dma_paths_busy(&tb,2,3)==0); /* non-DMA never owns DMA IDs */
    tb.cm.tunnel_list=&tunnel;
    assert(tb_dma_paths_busy(&tb,-1,-1)==1);
    assert(tb_dma_paths_busy(&tb,2,3)==1);
    assert(tb_dma_paths_busy(&tb,20,30)==0);
    assert(tb_dma_paths_busy(&tb,2,30)==1); /* OR, not pair-only matching */
    assert(tb_dma_paths_busy(&tb,20,3)==1);
    assert(tb_dma_paths_busy(&tb,-1,3)==1);
    assert(tb_dma_paths_busy(&tb,2,-1)==1);
    assert(tb_dma_paths_busy(&tb,-1,30)==0);
    assert(tb_dma_paths_busy(&tb,20,-1)==0);
    tunnel.paths[0]=NULL;
    assert(tb_dma_paths_busy(&tb,2,3)==1); /* RX-only leftover still blocks */
    assert(tb_dma_paths_busy(&tb,2,-1)==0);
    tunnel.paths[0]=&tx;tunnel.paths[1]=NULL;
    assert(tb_dma_paths_busy(&tb,2,3)==1); /* TX-only leftover still blocks */
    assert(tb_dma_paths_busy(&tb,-1,3)==0);
    tunnel.paths[0]=NULL;
    assert(tb_dma_paths_busy(&tb,-1,-1)==1); /* empty/invalid tunnel still owned */
    assert(tb_dma_paths_busy(&tb,2,3)==0);
}
static void list_scan(void) {
    struct tb tb={.lock=true,.root_switch=&tb};struct tb_port nhi={true},port={false};
    struct tb_path rx={.path_length=1,.hops={{&port,&nhi,9,33}}};
    struct tb_tunnel tail={.dma=true,.npaths=1,.paths={&rx}};
    struct tb_tunnel middle={.dma=true,.next=&tail};
    struct tb_tunnel first={.dma=false,.next=&middle};tb.cm.tunnel_list=&first;
    assert(tb_dma_paths_busy(&tb,2,33)==1);
    assert(tb_dma_paths_busy(&tb,2,3)==0);
    middle.next=NULL;
    assert(tb_dma_paths_busy(&tb,2,33)==0);
    assert(tb_dma_paths_busy(&tb,-1,-1)==1);
}
int main(int argc,char **argv) {
    assert(argc==2);
    if(!strcmp(argv[1],"filters"))query_filters();
    else if(!strcmp(argv[1],"list"))list_scan();
    else return 2;
    puts("PASS actual DMA inventory callback");return 0;
}
