/* SPDX-License-Identifier: GPL-2.0-only */
/* Executes the actual notification helper; no packet/MMIO emulation. */
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
typedef uint32_t u32;
#define XDOMAIN_RETRIES 10
#define XDOMAIN_SHORT_TIMEOUT 100
#define XDOMAIN_DEFAULT_TIMEOUT 1000
#define container_of(p,t,m) ((t *)((char *)(p)-offsetof(t,m)))
#define READ_ONCE(x) (x)
#define msecs_to_jiffies(x) (x)
#define dev_warn(...) ((void)0)
struct mutex { pthread_mutex_t m; };
static void mutex_lock(struct mutex *m) { assert(!pthread_mutex_lock(&m->m)); }
static void mutex_unlock(struct mutex *m) { assert(!pthread_mutex_unlock(&m->m)); }
#define lockdep_assert_held(x) ((void)(x))
struct work_struct { int unused; };
struct delayed_work { struct work_struct work; bool pending; unsigned queues; };
struct tb { void *wq, *ctl; };
struct tb_xdomain {
    struct mutex lock; struct tb *tb; struct delayed_work properties_changed_work;
    bool properties_post_enumeration, properties_notify_pending, properties_notify_stopped, removing, is_unplugged;
    u32 properties_notify_serial; int properties_changed_retries; unsigned long long route;
    void *local_uuid;
};
static struct tb tb;
static int status, calls, last_retry;
static struct tb_xdomain *request_owner;
static void (*request_hook)(void);
static bool queue_delayed_work(void *wq, struct delayed_work *work, unsigned delay) {
    (void)wq;assert(delay==100 || delay==1000);
    if(work->pending)return false;
    work->pending=true;++work->queues;return true;
}
static int tb_xdp_properties_changed_request(void *ctl, unsigned long long route,
                                           int retries, void *uuid) {
    (void)ctl;(void)route;(void)uuid;
    assert(retries>=0&&retries<=XDOMAIN_RETRIES);
    /* A synchronous request must not hold the lock needed by a local update. */
    assert(!pthread_mutex_trylock(&request_owner->lock.m));
    assert(!pthread_mutex_unlock(&request_owner->lock.m));
    ++calls;last_retry=retries;
    if(request_hook)request_hook();
    return status;
}
#include "xdomain_notify.h"
static void init(struct tb_xdomain *xd) {
    memset(xd,0,sizeof(*xd));assert(!pthread_mutex_init(&xd->lock.m,NULL));xd->tb=&tb;
}
static void fire(struct tb_xdomain *xd) {
    assert(xd->properties_changed_work.pending);
    xd->properties_changed_work.pending=false;request_owner=xd;
    tb_xdomain_properties_changed(&xd->properties_changed_work.work);
}
static void post_once(void) {
    struct tb_xdomain xd;init(&xd);tb_xdomain_notify_reset(&xd);
    for(int i=0;i<10000;++i)tb_xdomain_notify_enumerated(&xd);
    assert(xd.properties_changed_work.queues==1);
    fire(&xd);assert(calls==1&&!xd.properties_notify_pending);
    for(int i=0;i<10000;++i)tb_xdomain_notify_enumerated(&xd);
    assert(!xd.properties_changed_work.pending);
    tb_xdomain_notify_reset(&xd);tb_xdomain_notify_enumerated(&xd);
    fire(&xd);assert(calls==2);
}
static void budget(void) {
    struct tb_xdomain xd;init(&xd);status=-EIO;
    tb_xdomain_queue_properties_changed(&xd);
    for(int i=0;i<=XDOMAIN_RETRIES;++i){fire(&xd);assert(last_retry==XDOMAIN_RETRIES-i);}
    assert(calls==11&&!xd.properties_notify_pending&&!xd.properties_changed_work.pending);
    tb_xdomain_queue_properties_changed(&xd);status=0;fire(&xd);
    assert(calls==12&&last_retry==10&&!xd.properties_notify_pending);
}
static void update_in_request(void) { request_hook=NULL;tb_xdomain_queue_properties_changed(request_owner); }
static void concurrent_update(void) {
    struct tb_xdomain xd;init(&xd);
    tb_xdomain_queue_properties_changed(&xd);request_hook=update_in_request;
    fire(&xd);assert(xd.properties_notify_pending&&xd.properties_changed_retries==10);
    fire(&xd);assert(calls==2&&!xd.properties_notify_pending);
    status=-EIO;tb_xdomain_queue_properties_changed(&xd);request_hook=update_in_request;
    fire(&xd);assert(xd.properties_notify_pending&&xd.properties_changed_retries==10);
    status=0;fire(&xd);assert(calls==4&&!xd.properties_notify_pending);
}
static void reset_in_request(void) {request_hook=NULL;tb_xdomain_notify_reset(request_owner);}
static void stale_handshake(void) {
    struct tb_xdomain xd;init(&xd);tb_xdomain_queue_properties_changed(&xd);
    request_hook=reset_in_request;status=-EIO;fire(&xd);
    assert(!xd.properties_notify_pending&&!xd.properties_changed_work.pending);
    status=0;tb_xdomain_notify_enumerated(&xd);fire(&xd);assert(calls==2);
}
static void remove_in_request(void) {request_hook=NULL;request_owner->removing=true;}
static void removed(void) {
    struct tb_xdomain xd;init(&xd);tb_xdomain_queue_properties_changed(&xd);xd.removing=true;
    fire(&xd);assert(calls==0&&!xd.properties_notify_pending);
    tb_xdomain_notify_enumerated(&xd);assert(!xd.properties_changed_work.pending);
    xd.removing=false;xd.is_unplugged=true;tb_xdomain_queue_properties_changed(&xd);assert(!xd.properties_changed_work.pending);
    xd.is_unplugged=false;tb_xdomain_queue_properties_changed(&xd);request_hook=remove_in_request;
    status=-EIO;fire(&xd);assert(calls==1&&!xd.properties_notify_pending&&!xd.properties_changed_work.pending);
}
static struct tb_xdomain *peer;
static void remote_refresh(void) {tb_xdomain_notify_enumerated(peer);}
static void no_pingpong(void) {
    struct tb_xdomain a,b;init(&a);init(&b);
    tb_xdomain_notify_enumerated(&a);tb_xdomain_notify_enumerated(&b);
    request_hook=remote_refresh;peer=&b;fire(&a);peer=&a;fire(&b);
    assert(calls==2&&!a.properties_changed_work.pending&&!b.properties_changed_work.pending);
}
static void stop_in_request(void) {request_hook=NULL;tb_xdomain_notify_stop(request_owner);}
static void suspend_resume(void) {
    struct tb_xdomain xd;init(&xd);
    tb_xdomain_queue_properties_changed(&xd);
    request_hook=stop_in_request;status=-EIO;fire(&xd);
    assert(!xd.properties_notify_pending && !xd.properties_changed_work.pending);
    for(int i=0;i<1000;++i){tb_xdomain_queue_properties_changed(&xd);tb_xdomain_notify_enumerated(&xd);}
    assert(calls==1 && !xd.properties_changed_work.pending);
    tb_xdomain_notify_reset(&xd);tb_xdomain_notify_enumerated(&xd);
    status=0;fire(&xd);assert(calls==2 && !xd.properties_notify_pending);
}
int main(int argc,char **argv) {
    assert(argc==2);
    if(!strcmp(argv[1],"post_once"))post_once();
    else if(!strcmp(argv[1],"budget"))budget();
    else if(!strcmp(argv[1],"concurrent_update"))concurrent_update();
    else if(!strcmp(argv[1],"stale_handshake"))stale_handshake();
    else if(!strcmp(argv[1],"removed"))removed();
    else if(!strcmp(argv[1],"no_pingpong"))no_pingpong();
    else if(!strcmp(argv[1],"suspend_resume"))suspend_resume();
    else return 2;
    printf("PASS %s\n",argv[1]);return 0;
}
