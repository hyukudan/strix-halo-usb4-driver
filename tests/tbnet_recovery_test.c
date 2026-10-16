/* SPDX-License-Identifier: GPL-2.0-only */
/* Actual network recovery body, standard netdevice/RTNL operations substituted. */
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <pthread.h>
#define GFP_KERNEL 0
#define TB_DMA_GROUP_RECOVERY 2UL
#define NOTIFY_DONE 0
#define NOTIFY_OK 1
#define READ_ONCE(x) (x)
struct list_head { struct list_head *next, *prev; };
#define LIST_HEAD(name) struct list_head name = {&name,&name}
#define container_of(p,t,m) ((t *)((char *)(p)-offsetof(t,m)))
#define list_for_each_entry(pos,head,member) \
 for(pos=container_of((head)->next,__typeof__(*pos),member); &pos->member!=(head);pos=container_of(pos->member.next,__typeof__(*pos),member))
static void INIT_LIST_HEAD(struct list_head *h) {h->next=h->prev=h;}
static void list_add_tail(struct list_head *n,struct list_head *h){n->prev=h->prev;n->next=h;h->prev->next=n;h->prev=n;}
static void list_del_init(struct list_head *n){n->next->prev=n->prev;n->prev->next=n->next;INIT_LIST_HEAD(n);}
static bool rtnl_owned, lock_busy, allocation_fails;
static void rtnl_lock(void){assert(!rtnl_owned);rtnl_owned=true;}
static bool rtnl_trylock(void){if(lock_busy || rtnl_owned)return false;rtnl_owned=true;return true;}
static void rtnl_unlock(void){assert(rtnl_owned);rtnl_owned=false;}
static void *kcalloc(size_t n,size_t b,int flags){(void)flags;return allocation_fails?NULL:calloc(n,b);}
#define kfree free
struct tb {int id;};
struct tb_xdomain {struct tb *tb;bool is_unplugged;};
struct tbnet;
struct net_device {struct tbnet *net;bool up,present;unsigned closes,opens;bool keep_ring;int open_error;};
struct tbnet {struct tb_xdomain *xd;struct net_device *dev;struct list_head recovery_node;struct {void *ring;} tx_ring,rx_ring;};
static bool netif_running(struct net_device *d){return d->up;}
static bool netif_device_present(struct net_device *d){return d->present;}
static void dev_close(struct net_device *d){assert(rtnl_owned && d->up);d->up=false;++d->closes;if(!d->keep_ring)d->net->tx_ring.ring=d->net->rx_ring.ring=NULL;}
static int dev_open(struct net_device *d,void *extack){assert(rtnl_owned&&!d->up&&!extack);++d->opens;if(d->open_error)return d->open_error;d->up=true;d->net->tx_ring.ring=d->net->rx_ring.ring=d;return 0;}
struct notifier_block {int (*notifier_call)(struct notifier_block*,unsigned long,void*);};
static struct notifier_block *registered;
static int tb_register_dma_recovery_notifier(struct notifier_block *n){assert(!registered);registered=n;return 0;}
static int tb_unregister_dma_recovery_notifier(struct notifier_block *n){assert(registered==n);registered=NULL;return 0;}
struct tb_dma_reset_request {struct tb *tb;bool *performed;bool handled;int status;int(*execute)(struct tb_dma_reset_request*);};
/* SOURCE_INSERT */
struct fixture {struct tbnet net;struct tb_xdomain xd;struct net_device device;};
static void init(struct fixture *f,struct tb *domain,bool up){memset(f,0,sizeof(*f));f->xd.tb=domain;f->net.xd=&f->xd;f->net.dev=&f->device;f->device.net=&f->net;f->device.up=up;f->device.present=true;INIT_LIST_HEAD(&f->net.recovery_node);if(up)f->net.tx_ring.ring=f->net.rx_ring.ring=f;tbnet_dma_register_device(&f->net);}
static int resets,reset_status;
static bool reset_started=true;
static int execute(struct tb_dma_reset_request *r){assert(rtnl_owned);++resets;struct tbnet *n;list_for_each_entry(n,&tbnet_dma_clients,recovery_node)if(n->xd->tb==r->tb)assert(!n->dev->up&&!n->tx_ring.ring&&!n->rx_ring.ring);*r->performed=reset_started;return reset_status;}
static void scenario(const char *name){struct tb domain={1},other={2};struct fixture a,b,c;init(&a,&domain,true);init(&b,&domain,false);init(&c,&other,true);bool performed=false;struct tb_dma_reset_request r={.tb=&domain,.performed=&performed,.execute=execute};
 if(!strcmp(name,"success")){
  assert(tbnet_dma_lifecycle_init()==0);assert(tbnet_dma_recovery_notify(NULL,TB_DMA_GROUP_RECOVERY,&r)==NOTIFY_OK);assert(!r.status&&performed&&resets==1);assert(a.device.up&&a.device.closes==1&&a.device.opens==1);assert(!b.device.closes&&!b.device.opens&&!b.device.up);assert(c.device.up&&!c.device.closes);assert(tbnet_dma_recovery_notify(NULL,TB_DMA_GROUP_RECOVERY,&r)==NOTIFY_DONE);tbnet_dma_lifecycle_exit();assert(!registered);
 } else if(!strcmp(name,"preflight")){
  c.xd.tb=&domain;c.xd.is_unplugged=true;tbnet_dma_recovery_notify(NULL,TB_DMA_GROUP_RECOVERY,&r);assert(r.status==-ENODEV&&!resets&&!a.device.closes);
 } else if(!strcmp(name,"busy")){
  lock_busy=true;tbnet_dma_recovery_notify(NULL,TB_DMA_GROUP_RECOVERY,&r);assert(r.status==-EAGAIN&&!resets&&!a.device.closes);lock_busy=false;
 } else if(!strcmp(name,"alloc")){
  allocation_fails=true;tbnet_dma_recovery_notify(NULL,TB_DMA_GROUP_RECOVERY,&r);assert(r.status==-ENOMEM&&!resets&&!a.device.closes);allocation_fails=false;
 } else if(!strcmp(name,"reject_restore")){
  reset_started=false;reset_status=-EBUSY;tbnet_dma_recovery_notify(NULL,TB_DMA_GROUP_RECOVERY,&r);assert(r.status==-EBUSY&&resets==1&&!performed&&a.device.up&&a.device.opens==1);
 } else if(!strcmp(name,"failed_reset")){
  reset_status=-EIO;tbnet_dma_recovery_notify(NULL,TB_DMA_GROUP_RECOVERY,&r);assert(r.status==-EIO&&performed&&!a.device.up&&!a.device.opens);
 } else if(!strcmp(name,"failed_stop")){
  a.device.keep_ring=true;tbnet_dma_recovery_notify(NULL,TB_DMA_GROUP_RECOVERY,&r);assert(r.status==-EBUSY&&!resets&&!performed&&!a.device.up&&!a.device.opens&&a.net.tx_ring.ring);
 } else if(!strcmp(name,"failed_restore")){
  a.device.open_error=-ENODEV;tbnet_dma_recovery_notify(NULL,TB_DMA_GROUP_RECOVERY,&r);assert(r.status==-ENODEV&&performed&&!a.device.up&&a.device.opens==1);
 } else if(!strcmp(name,"empty")){
  a.xd.tb=&other;tbnet_dma_recovery_notify(NULL,TB_DMA_GROUP_RECOVERY,&r);assert(!r.status&&resets==1&&!a.device.closes);
 } else if(!strcmp(name,"unrelated_event")){
  assert(tbnet_dma_recovery_notify(NULL,1,&r)==NOTIFY_DONE);assert(!r.handled&&!resets&&!a.device.closes);
 } else abort();
 assert(!rtnl_owned);tbnet_dma_unregister_device(&a.net);tbnet_dma_unregister_device(&b.net);tbnet_dma_unregister_device(&c.net);assert(tbnet_dma_clients.next==&tbnet_dma_clients);}
int main(int argc,char **argv){assert(argc==2);scenario(argv[1]);printf("PASS %s\n",argv[1]);}
