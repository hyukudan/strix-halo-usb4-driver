/* SPDX-License-Identifier: GPL-2.0-only */
/* Tests invoke the actual driver functions above, with API/hardware doubles. */
struct fixture {
    struct tbstream_dev sdev;
    struct file file;
    struct tbstream stream;
    struct tb_service svc;
    struct tb_xdomain xd;
    struct tb tb;
    struct tb_cm_ops cm;
    struct tb_nhi nhi;
    uuid_t local,peer;
    struct dma_buf buffers[2];
    struct dma_resv resv[2];
    struct sg_table tables[2];
    struct scatterlist segments[2];
    struct file buffer_files[2];
    struct inode inodes[2];
    struct super_block sb;
};
static struct sgu_import import_request(struct fixture *f,u64 epoch) {
    struct sgu_import req={0};
    req.fence=f->sdev.gpu_context->identity;
    req.fence.connection_epoch=epoch;req.fence.pool_binding_identity=epoch+100;
    for(unsigned d=0;d<2;++d) {
        struct sgu_buffer *b=d?&req.rx:&req.tx;
        b->fd=d+1;b->full_bytes=f->buffers[d].size;b->window_bytes=b->full_bytes;
        b->allocation_generation=1;b->fd_device=f->sb.s_dev;b->fd_inode=f->inodes[d].i_ino;
    }
    return req;
}
static void fixture_open(struct fixture *f,unsigned depth,unsigned frames,bool import) {
    memset(f,0,sizeof(*f));
    mutex_init(&f->sdev.lock);
    f->sdev.ring_size=frames;f->sdev.stream=&f->stream;f->sdev.in_hopid=8;f->sdev.out_hopid=9;
    f->file.private_data=&f->sdev;
    f->stream.svc=&f->svc;f->svc.xd=&f->xd;f->xd.tb=&f->tb;
    f->tb.cm_ops=&f->cm;f->tb.nhi=&f->nhi;f->xd.local_uuid=&f->local;f->xd.remote_uuid=&f->peer;
    f->xd.route=1;f->sb.s_dev=55;
    assert(tbstream_gpu_open(&f->sdev)==0);
    for(unsigned d=0;d<2;++d) {
        mutex_init(&f->resv[d].lock);
        f->buffers[d].size=(u64)frames*SGU_FRAME_BYTES;
        f->buffers[d].resv=&f->resv[d];f->buffers[d].file=&f->buffer_files[d];
        atomic_init(&f->buffers[d].refs,1);
        f->buffer_files[d].inode=&f->inodes[d];f->inodes[d].i_sb=&f->sb;f->inodes[d].i_ino=d+10;
        f->segments[d].address=0x100000+(u64)d*0x1000000;
        f->segments[d].bytes=f->buffers[d].size;
        f->tables[d].nents=1;f->tables[d].sgl=&f->segments[d];f->buffers[d].mapping=&f->tables[d];
        dma_buffers[d+1]=&f->buffers[d];
    }
    struct sgu_queue_config q={.fence=f->sdev.gpu_context->identity,.depth=depth};
    assert(tbstream_gpu_ioctl(&f->file,SGU_IOC_QUEUE,(unsigned long)&q)==0);
    assert(q.depth==depth&&q.maximum_depth==SGU_MAX_QUEUE_DEPTH);
    if(import) {
        struct sgu_import req=import_request(f,1);
        assert(tbstream_gpu_ioctl(&f->file,SGU_IOC_IMPORT,(unsigned long)&req)==0);
        struct sgu_action a={.fence=req.fence};
        assert(tbstream_gpu_ioctl(&f->file,SGU_IOC_ARM,(unsigned long)&a)==0);
    }
}
static void fixture_close(struct fixture *f) {
    mutex_lock(&f->sdev.lock);tbstream_gpu_close(&f->sdev);mutex_unlock(&f->sdev.lock);
    assert(f->sdev.gpu_context==NULL);
    for(unsigned d=0;d<2;++d) {
        resv_clear(&f->resv[d]);assert(atomic_load(&f->buffers[d].refs)==1);
        dma_buffers[d+1]=NULL;pthread_mutex_destroy(&f->resv[d].lock.m);
    }
    pthread_mutex_destroy(&f->sdev.lock.m);
}
static int prepare(struct fixture *f,unsigned d,unsigned slot,u64 handle,u64 seq,u64 offset,u64 bytes) {
    struct sgu_prepare_slot p={0};
    p.operation=(struct sgu_prepare){.fence=f->sdev.gpu_context->identity,.handle=handle,
        .sequence=seq,.pool_offset=offset,.payload_bytes=bytes,.terminal_bytes=128,.direction=d};
    p.slot=slot;
    return tbstream_gpu_ioctl(&f->file,SGU_IOC_PREPARE_SLOT,(unsigned long)&p);
}
static int action(struct fixture *f,unsigned cmd,unsigned d,u64 handle,u64 seq) {
    struct sgu_action a={.fence=f->sdev.gpu_context->identity,.handle=handle,.sequence=seq,.direction=d};
    return tbstream_gpu_ioctl(&f->file,cmd,(unsigned long)&a);
}
static void complete_one(struct tb_ring *r) {
    assert(!list_empty(&r->queue));
    struct ring_frame *rf=container_of(r->queue.next,struct ring_frame,list);
    struct sgu_hw_frame *sf=container_of(rf,struct sgu_hw_frame,frame);
    rf->size=sf->result.expected_bytes==4096?0:sf->result.expected_bytes;
    rf->sof=sf->result.expected_bytes<=256?0:1;rf->eof=2;
    list_del_init(&rf->list);rf->callback(r,rf,false);
}
static void complete_all(struct tb_ring *r) { while(!list_empty(&r->queue))complete_one(r); }
static void consume_event(struct fixture *f,unsigned expected_direction,u64 expected_handle,unsigned frames) {
    unsigned cursor=0;
    while(cursor<frames) {
        struct sgu_event e={.fence=f->sdev.gpu_context->identity};
        assert(tbstream_gpu_ioctl(&f->file,SGU_IOC_EVENT,(unsigned long)&e)==0);
        assert(e.direction==expected_direction&&e.handle==expected_handle&&e.first_frame==cursor);
        assert(e.total_frames==frames&&e.count>0&&e.count<=SGU_EVENT_BATCH);
        assert(e.status==SGU_EVENT_COMPLETE);
        cursor+=e.count;
    }
}
static struct dma_fence *foreign_fence(struct dma_resv *resv) {
    static const struct dma_fence_ops ops={.release=dma_fence_free};
    struct dma_fence *f=calloc(1,sizeof(*f));assert(f);
    dma_fence_init(f,&ops,NULL,dma_fence_context_alloc(1),1);
    dma_resv_add_fence(resv,f,DMA_RESV_USAGE_WRITE);return f;
}
static void test_geometry(void) {
    unsigned values[]={32,64,256,4096};u32 count;
    for(unsigned i=0;i<4;++i)for(unsigned depth=1;depth<=8;++depth) {
        unsigned n=values[i];struct sgu_prepare p={.handle=1,.sequence=1,.terminal_bytes=1,.payload_bytes=(u64)(n-depth)*4096};
        assert(sgu_prepare_queue_valid(&p,n,depth,&count));assert(count==n-depth);
        ++p.payload_bytes;assert(!sgu_prepare_queue_valid(&p,n,depth,&count));
        p.pool_offset=(u64)(n-depth-1)*4096;p.payload_bytes=4096;
        assert(sgu_prepare_queue_valid(&p,n,depth,&count)&&count==1);
        p.pool_offset+=4096;assert(!sgu_prepare_queue_valid(&p,n,depth,&count));
        p.pool_offset=U64_MAX;assert(!sgu_prepare_queue_valid(&p,n,depth,&count));
        p.pool_offset=0;p.payload_bytes=U64_MAX;assert(!sgu_prepare_queue_valid(&p,n,depth,&count));
    }
    for(unsigned a=0;a<100;++a)for(unsigned na=1;na<12;++na)
        for(unsigned b=0;b<100;++b)for(unsigned nb=1;nb<12;++nb)
            assert(sgu_frame_ranges_overlap(a*4096,na,b*4096,nb)==(a<b+nb&&b<a+na));
    assert(!sgu_owns_fence_context(100,99));assert(sgu_owns_fence_context(100,115));
    assert(!sgu_owns_fence_context(100,116));
    struct fixture f;fixture_open(&f,4,64,true);
    assert(prepare(&f,0,0,1,1,0,1)==0);
    assert(prepare(&f,0,1,2,2,0,1)==-EBUSY);
    assert(prepare(&f,0,0,2,2,4096,1)==-EBUSY);
    assert(prepare(&f,0,1,2,2,4096,1)==0);
    assert(prepare(&f,0,4,3,3,8192,1)==-EINVAL);
    assert(prepare(&f,0,2,3,3,60*4096,1)==-EINVAL);
    struct sgu_prepare old={0};assert(tbstream_gpu_ioctl(&f.file,SGU_IOC_PREPARE,(unsigned long)&old)==-EOPNOTSUPP);
    fixture_close(&f);assert(atomic_load(&module_refs)==0);
}
static void test_queue(void) {
    struct fixture f;fixture_open(&f,4,64,true);
    struct tbstream_gpu_context *ctx=f.sdev.gpu_context;
    for(unsigned d=0;d<2;++d)for(unsigned slot=0;slot<4;++slot) {
        unsigned h=d*4+slot+1;
        assert(prepare(&f,d,slot,h,slot+1,(u64)slot*4*4096,4*4096-1)==0);
        assert(action(&f,SGU_IOC_SUBMIT,d,h,slot+1)==0);
        struct sgu_hw_operation *op=&ctx->operations[d][slot];
        assert(sgu_frame_at(ctx,op,op->payload_frames)->result.physical_slot==60+slot);
        assert(!dma_fence_is_signaled(op->fence));
        for(unsigned other=0;other<slot;++other)assert(op->fence->context!=ctx->operations[d][other].fence->context);
    }
    assert(ctx->lifetime.pending_callbacks==40);assert(f.resv[0].count==4&&f.resv[1].count==4);
    assert(action(&f,SGU_IOC_RX_REPOST,1,5,1)==-EOPNOTSUPP);
    int before=wakeups;complete_all(f.sdev.tx_ring.ring);complete_all(f.sdev.rx_ring.ring);
    assert(wakeups-before==8);assert(ctx->lifetime.pending_callbacks==0);
    for(unsigned d=0;d<2;++d)for(unsigned slot=0;slot<4;++slot) {
        consume_event(&f,d,d*4+slot+1,5);
        assert(action(&f,SGU_IOC_RETIRE,d,d*4+slot+1,slot+1)==0);
    }
    assert(f.sdev.rx_ring.ring->running&&f.sdev.tx_ring.ring->running);
    assert(prepare(&f,1,0,9,5,0,1)==0);assert(action(&f,SGU_IOC_SUBMIT,1,9,5)==0);
    fixture_close(&f);assert(atomic_load(&module_refs)==0);
}
static void test_failure_isolation(void) {
    struct fixture f;fixture_open(&f,4,64,true);
    assert(prepare(&f,1,0,1,1,0,8192)==0);assert(action(&f,SGU_IOC_SUBMIT,1,1,1)==0);
    struct dma_fence *first=dma_fence_get(f.sdev.gpu_context->operations[1][0].fence);
    assert(prepare(&f,1,1,2,2,8192,8192)==0);enqueue_fail=1;
    assert(action(&f,SGU_IOC_SUBMIT,1,2,2)==-ESHUTDOWN);
    assert(!dma_fence_is_signaled(first));assert(f.resv[1].count==2);
    assert(f.sdev.gpu_context->lifetime.pending_callbacks==3);
    assert(dma_fence_is_signaled(f.sdev.gpu_context->operations[1][1].fence));
    run_delayed_work(&f.sdev.gpu_context->watchdog);
    assert(dma_fence_is_signaled(first)&&first->error==-ECANCELED);
    dma_fence_put(first);fixture_close(&f);assert(atomic_load(&module_refs)==0);
}
static void test_events_and_legacy(void) {
    struct fixture f;fixture_open(&f,1,256,true);
    assert(prepare(&f,0,0,1,1,0,80*4096)==0);assert(action(&f,SGU_IOC_SUBMIT,0,1,1)==0);
    complete_all(f.sdev.tx_ring.ring);
    struct sgu_event e={.fence=f.sdev.gpu_context->identity};copy_to_fail=1;
    assert(tbstream_gpu_ioctl(&f.file,SGU_IOC_EVENT,(unsigned long)&e)==-EFAULT);
    assert(f.sdev.gpu_context->operations[0][0].event_cursor==0);
    consume_event(&f,0,1,81);assert(action(&f,SGU_IOC_RETIRE,0,1,1)==0);
    assert(prepare(&f,1,0,2,1,0,8192)==0);assert(action(&f,SGU_IOC_SUBMIT,1,2,1)==0);
    complete_all(f.sdev.rx_ring.ring);consume_event(&f,1,2,3);
    assert(action(&f,SGU_IOC_RX_REPOST,1,2,1)==0);assert(action(&f,SGU_IOC_RETIRE,1,2,1)==0);
    unsigned pending=f.sdev.gpu_context->lifetime.pending_callbacks;
    assert(prepare(&f,1,0,3,2,4096,8192)==-EOPNOTSUPP);
    assert(prepare(&f,1,0,3,2,0,8191)==0);
    assert(prepare(&f,1,0,4,3,0,8191)==-EBUSY); /* no second PREPARE on reused RX */
    assert(action(&f,SGU_IOC_SUBMIT,1,3,2)==0);
    assert(f.sdev.gpu_context->lifetime.pending_callbacks==pending);
    complete_all(f.sdev.rx_ring.ring);consume_event(&f,1,3,3);
    /* Reposted slots are physically owned before SUBMIT. An unexpected peer
     * frame during PREPARE must fail closed, never resurrect a completed op. */
    assert(action(&f,SGU_IOC_RX_REPOST,1,3,2)==0);
    assert(action(&f,SGU_IOC_RETIRE,1,3,2)==0);
    assert(prepare(&f,1,0,4,3,0,8191)==0);
    complete_all(f.sdev.rx_ring.ring);
    assert(f.sdev.gpu_context->failed);
    assert(action(&f,SGU_IOC_SUBMIT,1,4,3)==-ESHUTDOWN);
    assert(f.sdev.gpu_context->operations[1][0].phase==SGU_OP_COMPLETE);
    fixture_close(&f);assert(atomic_load(&module_refs)==0);
}
static void test_timeout_recovery(void) {
    struct fixture f;fixture_open(&f,2,64,true);
    struct tbstream_gpu_context *ctx=f.sdev.gpu_context;
    assert(prepare(&f,1,0,1,1,0,8192)==0);assert(action(&f,SGU_IOC_SUBMIT,1,1,1)==0);
    struct dma_fence *old=dma_fence_get(ctx->operations[1][0].fence);
    struct sgu_fence identity=ctx->identity;
    ctx->operations[1][0].deadline_ns=0;
    int before=unmaps;stop_fails=true;run_delayed_work(&ctx->watchdog);
    assert(ctx->failed&&!ctx->lifetime.stop_confirmed&&!dma_fence_is_signaled(old));
    assert(unmaps==before);assert(action(&f,SGU_IOC_RECOVER,0,0,0)==-EIO);assert(unmaps==before);
    stop_fails=false;
    assert(action(&f,SGU_IOC_RECOVER,0,0,0)==0);
    assert(!ctx->failed&&!ctx->imported&&ctx->requires_new_epoch);
    assert(dma_fence_is_signaled(old)&&old->error==-ETIMEDOUT);dma_fence_put(old);
    struct sgu_import req=import_request(&f,1);
    assert(tbstream_gpu_ioctl(&f.file,SGU_IOC_IMPORT,(unsigned long)&req)==-EINVAL);
    req=import_request(&f,2);assert(tbstream_gpu_ioctl(&f.file,SGU_IOC_IMPORT,(unsigned long)&req)==0);
    assert(req.fence.import_generation>identity.import_generation);
    struct sgu_event e={.fence=identity};assert(tbstream_gpu_ioctl(&f.file,SGU_IOC_EVENT,(unsigned long)&e)==-ESTALE);
    fixture_close(&f);assert(atomic_load(&module_refs)==0);
}
struct submit_thread { struct fixture *f;struct sgu_action a;int result; };
static void *submit_worker(void *data) {
    struct submit_thread *t=data;t->result=tbstream_gpu_ioctl(&t->f->file,SGU_IOC_SUBMIT,(unsigned long)&t->a);return NULL;
}
static void wait_for_fence_waiter(void) {
    unsigned long until=jiffies+1000;
    while(!atomic_load(&fence_waiters)&&!time_after_eq(jiffies,until))usleep(500);
    assert(atomic_load(&fence_waiters)>0);
}
static void test_wait_concurrency(void) {
    struct fixture f;fixture_open(&f,2,64,true);
    struct dma_fence *foreign=foreign_fence(&f.resv[0]);
    assert(prepare(&f,0,0,1,1,0,4096)==0);
    f.file.f_flags=O_NONBLOCK;int before=dma_syncs;
    assert(action(&f,SGU_IOC_SUBMIT,0,1,1)==-EAGAIN);
    assert(f.sdev.gpu_context->operations[0][0].fence==NULL&&dma_syncs==before);
    f.file.f_flags=0;
    struct submit_thread t={.f=&f,.a={.fence=f.sdev.gpu_context->identity,.handle=1,.sequence=1}};
    pthread_t worker;assert(!pthread_create(&worker,NULL,submit_worker,&t));wait_for_fence_waiter();
    /* Opposite-direction preparation and publication must not wait for TX's GPU. */
    assert(prepare(&f,1,0,2,1,0,4096)==0);assert(action(&f,SGU_IOC_SUBMIT,1,2,1)==0);
    assert(action(&f,SGU_IOC_STOP,0,0,0)==0);assert(action(&f,SGU_IOC_RELEASE,0,0,0)==0);
    assert(!pthread_join(worker,NULL));assert(t.result==-ESTALE);
    dma_fence_signal(foreign);dma_fence_put(foreign);
    fixture_close(&f);assert(atomic_load(&module_refs)==0);
}
static void test_wait_retry_and_order(void) {
    struct fixture f;fixture_open(&f,2,64,true);
    struct dma_fence *foreign=foreign_fence(&f.resv[0]);
    assert(prepare(&f,0,0,1,1,0,4096)==0);
    struct submit_thread t={.f=&f,.a={.fence=f.sdev.gpu_context->identity,.handle=1,.sequence=1}};
    pthread_t worker;assert(!pthread_create(&worker,NULL,submit_worker,&t));wait_for_fence_waiter();
    dma_fence_signal(foreign);assert(!pthread_join(worker,NULL));assert(t.result==0);
    dma_fence_put(foreign);
    assert(prepare(&f,1,0,2,1,0,4096)==0);assert(prepare(&f,1,1,3,2,4096,4096)==0);
    assert(action(&f,SGU_IOC_SUBMIT,1,3,2)==0);
    assert(action(&f,SGU_IOC_SUBMIT,1,2,1)==-ESTALE);
    fixture_close(&f);assert(atomic_load(&module_refs)==0);
}
static void test_export_lifetime(void) {
    struct fixture f;fixture_open(&f,2,64,true);
    assert(prepare(&f,0,0,1,1,0,4096)==0);
    struct sgu_sync_file request={.fence=f.sdev.gpu_context->identity,.handle=1,.sequence=1,.fd=-1};
    assert(tbstream_gpu_ioctl(&f.file,SGU_IOC_EXPORT_SYNC_FILE,(unsigned long)&request)==-EINVAL);
    assert(action(&f,SGU_IOC_SUBMIT,0,1,1)==0);
    fd_fail=1;assert(tbstream_gpu_ioctl(&f.file,SGU_IOC_EXPORT_SYNC_FILE,(unsigned long)&request)==-EMFILE);
    alloc_fail_after=0;assert(tbstream_gpu_ioctl(&f.file,SGU_IOC_EXPORT_SYNC_FILE,(unsigned long)&request)==-ENOMEM);
    copy_to_fail=1;assert(tbstream_gpu_ioctl(&f.file,SGU_IOC_EXPORT_SYNC_FILE,(unsigned long)&request)==-EFAULT);
    for(int i=0;i<256;++i)assert(!fd_reserved[i]&&!fd_table[i]);
    assert(tbstream_gpu_ioctl(&f.file,SGU_IOC_EXPORT_SYNC_FILE,(unsigned long)&request)==0);
    int fd=request.fd;assert(fd_table[fd]&&(fd_table[fd]->f_flags&O_CLOEXEC));
    struct dma_fence *exported=fd_table[fd]->sync->fence;
    fixture_close(&f);
    assert(atomic_load(&module_refs)==1&&dma_fence_is_signaled(exported));
    fput(fd_table[fd]);fd_table[fd]=NULL;assert(atomic_load(&module_refs)==0);
}
static void test_admission_and_rollback(void) {
    struct fixture f;fixture_open(&f,2,64,false);
    need_sync=true;struct sgu_import req=import_request(&f,1);
    assert(tbstream_gpu_ioctl(&f.file,SGU_IOC_IMPORT,(unsigned long)&req)==-EOPNOTSUPP);
    assert(!f.sdev.gpu_context->imported&&atomic_load(&f.buffers[0].refs)==1);
    need_sync=false;assert(tbstream_gpu_ioctl(&f.file,SGU_IOC_IMPORT,(unsigned long)&req)==0);
    assert(action(&f,SGU_IOC_ARM,0,0,0)==0);
    struct sgu_queue_config q={.fence=req.fence,.depth=4};
    assert(tbstream_gpu_ioctl(&f.file,SGU_IOC_QUEUE,(unsigned long)&q)==-EBUSY);
    assert(prepare(&f,0,0,1,1,0,4096)==0);reserve_fail=1;
    assert(action(&f,SGU_IOC_SUBMIT,0,1,1)==-ENOMEM);
    assert(f.sdev.gpu_context->operations[0][0].fence==NULL&&atomic_load(&module_refs)==0);
    assert(action(&f,SGU_IOC_SUBMIT,0,1,1)==0);
    mutex_lock(&f.sdev.lock);run_delayed_work(&f.sdev.gpu_context->watchdog);
    assert(atomic_load(&f.sdev.gpu_context->watchdog.scheduled));mutex_unlock(&f.sdev.lock);
    fixture_close(&f);assert(atomic_load(&module_refs)==0);
    unsigned value=500;struct kernel_param kp={.arg=&value};
    assert(sgu_timeout_set("0",&kp)==-ERANGE&&value==500);
    assert(sgu_timeout_set("3600001",&kp)==-ERANGE);assert(sgu_timeout_set("100",&kp)==0&&value==100);
}

static void test_watchdog_schedule(void) {
    struct fixture f;fixture_open(&f,2,64,true);
    assert(prepare(&f,0,0,1,1,0,4096)==0);assert(action(&f,SGU_IOC_SUBMIT,0,1,1)==0);
    unsigned long due=f.sdev.gpu_context->watchdog.due;
    usleep(3000);
    assert(prepare(&f,1,0,2,1,0,4096)==0);assert(action(&f,SGU_IOC_SUBMIT,1,2,1)==0);
    assert(f.sdev.gpu_context->watchdog.due==due); /* new traffic cannot postpone old deadline */
    assert(action(&f,SGU_IOC_CANCEL,0,1,1)==0);
    run_delayed_work(&f.sdev.gpu_context->watchdog);
    assert(f.sdev.gpu_context->stopped&&f.sdev.gpu_context->lifetime.pending_callbacks==0);
    fixture_close(&f);assert(atomic_load(&module_refs)==0);
}
static void test_max_queue_reuse(void) {
    struct fixture f;fixture_open(&f,8,64,true);
    u64 handles[2][8],seq=0,handle=0;
    for(unsigned cycle=0;cycle<128;++cycle) {
        for(unsigned d=0;d<2;++d)for(unsigned slot=0;slot<8;++slot) {
            handles[d][slot]=++handle;
            assert(prepare(&f,d,slot,handle,seq+slot+1,(u64)slot*4*4096,4*4096-1)==0);
            assert(action(&f,SGU_IOC_SUBMIT,d,handle,seq+slot+1)==0);
            struct sgu_hw_operation *op=&f.sdev.gpu_context->operations[d][slot];
            assert(sgu_frame_at(f.sdev.gpu_context,op,4)->result.physical_slot==56+slot);
        }
        assert(f.sdev.gpu_context->lifetime.pending_callbacks==80);
        /* Both directions remain active; alternate completion order by cycle. */
        unsigned first=cycle%2;
        complete_all(first?f.sdev.rx_ring.ring:f.sdev.tx_ring.ring);
        complete_all(first?f.sdev.tx_ring.ring:f.sdev.rx_ring.ring);
        for(unsigned n=0;n<2;++n)for(unsigned slot=0;slot<8;++slot) {
            unsigned d=(first+n)%2;
            consume_event(&f,d,handles[d][slot],5);
            assert(action(&f,SGU_IOC_RETIRE,d,handles[d][slot],seq+slot+1)==0);
        }
        assert(f.sdev.gpu_context->lifetime.pending_callbacks==0);
        assert(f.resv[0].count==8&&f.resv[1].count==8);
        seq+=8;
    }
    fixture_close(&f);assert(atomic_load(&module_refs)==0);
}

#include "sgu_recovery_cases.h"

int main(int argc,char **argv) {
    assert(argc==2);
    struct { const char *name; void (*run)(void); } tests[]={
        {"geometry",test_geometry},{"queue",test_queue},{"failure_isolation",test_failure_isolation},
        {"events_legacy",test_events_and_legacy},{"timeout_recovery",test_timeout_recovery},
        {"wait_concurrency",test_wait_concurrency},{"wait_retry_order",test_wait_retry_and_order},
        {"export_lifetime",test_export_lifetime},{"admission_rollback",test_admission_and_rollback},
        {"watchdog_schedule",test_watchdog_schedule},{"max_queue_reuse",test_max_queue_reuse},
        {"watchdog_mutex_independence",test_watchdog_mutex_independence},
        {"fence_precedes_event",test_fence_precedes_event},
        {"stop_before_publication",test_stop_before_publication},
        {"quarantine_reaping",test_quarantine_reaping},
        {"explicit_controller_reset",test_explicit_controller_reset},
        {"statistics_snapshot",test_statistics_snapshot}};
    for(unsigned i=0;i<sizeof(tests)/sizeof(tests[0]);++i)if(!strcmp(tests[i].name,argv[1])) {
        tests[i].run();printf("PASS %s\n",argv[1]);return 0;
    }
    return 2;
}
