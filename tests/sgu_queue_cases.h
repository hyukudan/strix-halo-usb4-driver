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
static int fixture_gpu_poll; /* the endpoint's configfs gpu_poll for the next open */
static int fixture_qos_us = -1; /* its configfs latency_qos_us */
static void fixture_open(struct fixture *f,unsigned depth,unsigned frames,bool import) {
    memset(f,0,sizeof(*f));
    f->sdev.cfg_gpu_poll=fixture_gpu_poll;
    f->sdev.cfg_latency_qos_us=fixture_qos_us;
    mutex_init(&f->sdev.lock);
    f->sdev.ring_size=frames;f->sdev.stream=&f->stream;f->sdev.in_hopid=8;f->sdev.out_hopid=9;
    f->file.private_data=&f->sdev;
    f->stream.svc=&f->svc;f->svc.xd=&f->xd;f->xd.tb=&f->tb;
    f->tb.cm_ops=&f->cm;f->tb.nhi=&f->nhi;f->xd.local_uuid=&f->local;f->xd.remote_uuid=&f->peer;
    f->xd.route=1;f->sb.s_dev=55;
    f->nhi.dev.bound=true; f->nhi.dev.driver_data=&f->tb;
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
    /* As ring_work: an IRQ, the collection start and the reap, then callbacks */
    r->cb_stamps.irq_ns=ktime_get_ns();r->cb_stamps.start_ns=ktime_get_ns();r->cb_stamps.reap_ns=ktime_get_ns();
    ++r->irqs;++r->collections;
    list_del_init(&rf->list);rf->callback(r,rf,false);
}
static void complete_all(struct tb_ring *r) { while(!list_empty(&r->queue))complete_one(r); }
/* Polled rings: the controller finishes a frame, nobody is told. */
static void hw_complete_one(struct tb_ring *r) {
    assert(!list_empty(&r->queue));
    struct ring_frame *rf=container_of(r->queue.next,struct ring_frame,list);
    struct sgu_hw_frame *sf=container_of(rf,struct sgu_hw_frame,frame);
    rf->size=sf->result.expected_bytes==4096?0:sf->result.expected_bytes;
    rf->sof=sf->result.expected_bytes<=256?0:1;rf->eof=2;
    list_del_init(&rf->list);list_add_tail(&rf->list,&r->done);
}
static void hw_complete_all(struct tb_ring *r) { while(!list_empty(&r->queue))hw_complete_one(r); }
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
struct mutex_holder { struct mutex *m; atomic_int held; };
static void *hold_mutex(void *data) {
    struct mutex_holder *h=data;mutex_lock(h->m);atomic_store(&h->held,1);
    usleep(200000);mutex_unlock(h->m);return NULL;
}
/* Audit A9: O_NONBLOCK SUBMIT/RX_REPOST slept on the device mutex. */
static void test_nonblock_contended_mutex(void) {
    struct fixture f;fixture_open(&f,2,64,true);
    assert(prepare(&f,0,0,1,1,0,4096)==0);
    struct mutex_holder h={.m=&f.sdev.lock};pthread_t holder;
    assert(!pthread_create(&holder,NULL,hold_mutex,&h));
    while(!atomic_load(&h.held))usleep(100);
    f.file.f_flags=O_NONBLOCK;int before=dma_syncs;
    assert(action(&f,SGU_IOC_SUBMIT,0,1,1)==-EAGAIN);
    assert(action(&f,SGU_IOC_RX_REPOST,1,2,1)==-EAGAIN);
    assert(atomic_load(&h.held)&&dma_syncs==before);
    assert(!pthread_join(holder,NULL));
    assert(f.sdev.gpu_context->operations[0][0].fence==NULL);
    /* Uncontended, the same call publishes. */
    assert(action(&f,SGU_IOC_SUBMIT,0,1,1)==0);
    assert(f.sdev.gpu_context->operations[0][0].fence!=NULL);
    fixture_close(&f);assert(atomic_load(&module_refs)==0);
}
/* Review of A9: O_NONBLOCK SUBMIT/RX_REPOST still slept on ctx->dma_lock,
 * which the watchdog holds while it stops the rings and waits for them. Now
 * EAGAIN, with no fence published, no operation changed and the fence
 * sequence untouched. */
static void test_nonblock_contended_dma_lock(void) {
    struct fixture f;fixture_open(&f,1,256,true);
    struct tbstream_gpu_context *ctx=f.sdev.gpu_context;
    /* An RX slot ready for RX_REPOST, a TX slot ready for SUBMIT. */
    assert(prepare(&f,0,0,1,1,0,4096)==0);
    assert(prepare(&f,1,0,2,1,0,8192)==0);assert(action(&f,SGU_IOC_SUBMIT,1,2,1)==0);
    complete_all(f.sdev.rx_ring.ring);consume_event(&f,1,2,3);
    struct sgu_hw_operation *tx=&ctx->operations[0][0],*rx=&ctx->operations[1][0];
    struct dma_fence *rx_fence=rx->fence;
    u64 tx_seq=ctx->fence_sequence[0][0],rx_seq=ctx->fence_sequence[1][0];
    struct mutex_holder h={.m=&ctx->dma_lock};pthread_t holder;
    assert(!pthread_create(&holder,NULL,hold_mutex,&h));
    while(!atomic_load(&h.held))usleep(100);
    f.file.f_flags=O_NONBLOCK;int before=dma_syncs;
    assert(action(&f,SGU_IOC_SUBMIT,0,1,1)==-EAGAIN);
    assert(action(&f,SGU_IOC_RX_REPOST,1,2,1)==-EAGAIN);
    assert(atomic_load(&h.held)&&dma_syncs==before); /* returned while it was held */
    assert(!pthread_join(holder,NULL));
    assert(!tx->fence&&tx->phase==SGU_OP_PREPARED&&rx->fence==rx_fence&&rx->phase==SGU_OP_TAKEN);
    assert(ctx->fence_sequence[0][0]==tx_seq&&ctx->fence_sequence[1][0]==rx_seq);
    /* The publication itself finds it taken after SUBMIT's admission. */
    trylock_fail=&ctx->dma_lock;trylock_fail_skip=1;
    assert(action(&f,SGU_IOC_SUBMIT,0,1,1)==-EAGAIN&&!trylock_fail);
    assert(!tx->fence&&tx->phase==SGU_OP_PREPARED&&ctx->fence_sequence[0][0]==tx_seq&&dma_syncs==before);
    /* Uncontended, both go through. */
    assert(action(&f,SGU_IOC_SUBMIT,0,1,1)==0&&tx->fence&&ctx->fence_sequence[0][0]==tx_seq+1);
    assert(action(&f,SGU_IOC_RX_REPOST,1,2,1)==0&&rx->fence!=rx_fence);
    fixture_close(&f);assert(atomic_load(&module_refs)==0);
}
/* gpu_timing's page contract: NUL-terminated, less than a page, whole lines
 * only: every line ends in a newline, a stage line carries all its buckets
 * and a timing record all its offsets. */
static unsigned count_spaces(const char *c) { unsigned n=0;for(;*c;++c)n+=*c==' ';return n; }
static void assert_whole_lines(const char *buf,ssize_t len) {
    assert(len>0&&(size_t)len<PAGE_SIZE&&buf[len]=='\0'&&strlen(buf)==(size_t)len&&buf[len-1]=='\n');
    for(const char *l=buf;*l;) {
        const char *e=strchr(l,'\n');char line[PAGE_SIZE];const char *h;
        assert(e);memcpy(line,l,(size_t)(e-l));line[e-l]='\0';
        if((h=strstr(line," hist_us")))assert(count_spaces(h+8)==SGU_T_BUCKETS);
        if(!strncmp(line+2," last seq ",10)&&(h=strstr(line," ns")))assert(count_spaces(h+3)==SGU_T_STAGES);
        l=e+1;
    }
}
/* 0040: completion path timing, accounted when EVENT returns an operation. */
static void test_completion_timing(void) {
    struct fixture f;fixture_open(&f,1,256,true);
    struct tbstream_gpu_context *ctx=f.sdev.gpu_context;
    assert(prepare(&f,0,0,1,1,0,4096)==0);assert(action(&f,SGU_IOC_SUBMIT,0,1,1)==0);
    assert(ctx->operations[0][0].doorbell_ns>=ctx->operations[0][0].submitted_ns);
    complete_all(f.sdev.tx_ring.ring);
    assert(!ctx->timing[0].ops); /* only once EVENT hands it back */
    consume_event(&f,0,1,2);
    struct sgu_timing *t=&ctx->timing[0];
    assert(t->ops==1);
    for(unsigned s=0;s<SGU_T_STAGES;++s)assert(t->count[s]==1);
    assert(t->max_ns[SGU_T_TOTAL]>=t->max_ns[SGU_T_REAP]&&t->sum_ns[SGU_T_TOTAL]==t->max_ns[SGU_T_TOTAL]);
    unsigned buckets=0;for(unsigned b=0;b<SGU_T_BUCKETS;++b)buckets+=t->hist[SGU_T_TOTAL][b];
    assert(buckets==1&&t->last[0].sequence==1&&t->last[0].slot==0);
    assert(t->last[0].at[0]>=t->last[0].submit_ns&&t->last[0].at[6]>=t->last[0].at[5]);
    /* RX, with an IRQ older than its doorbell: no IRQ->work stage for it. */
    assert(prepare(&f,1,0,2,1,0,8192)==0);assert(action(&f,SGU_IOC_SUBMIT,1,2,1)==0);
    struct tb_ring *rx=f.sdev.rx_ring.ring;
    while(!list_empty(&rx->queue)) {
        complete_one(rx);
    }
    ctx->operations[1][0].irq_ns=1;
    consume_event(&f,1,2,3);
    t=&ctx->timing[1];
    assert(t->ops==1&&t->count[SGU_T_IRQ_WORK]==0&&t->count[SGU_T_REAP]==1&&t->count[SGU_T_TOTAL]==1);
    /* The read-out: one page, both directions, every stage. */
    char *buf=calloc(1,PAGE_SIZE);assert(buf);
    ssize_t len=sgu_timing_show(ctx,buf);
    assert(len>0&&(size_t)len<PAGE_SIZE-1&&strlen(buf)==(size_t)len);
    assert(strstr(buf,"mode irq")&&strstr(buf,"tx ops 1\n")&&strstr(buf,"rx ops 1\n"));
    assert(strstr(buf,"tx irq_work n 1 ")&&strstr(buf,"rx irq_work n 0 ")&&strstr(buf,"tx last seq 1 slot 0 ns "));
    assert(strstr(buf,"ring tx irqs ")&&strstr(buf,"ring rx irqs "));
    /* Filled to the maximum: still within a page. */
    for(unsigned d=0;d<2;++d){ctx->timing[d].ops=~0ULL>>1;ctx->timing[d].next=SGU_T_RECORDS;
        for(unsigned s=0;s<SGU_T_STAGES;++s){ctx->timing[d].count[s]=~0ULL>>1;ctx->timing[d].max_ns[s]=~0ULL>>1;
            for(unsigned b=0;b<SGU_T_BUCKETS;++b)ctx->timing[d].hist[s][b]=0xffffffffU;}}
    /* At these extremes (19-digit counters, 10-digit buckets) the read-out
     * does not fit a page: it ends at a line boundary, never inside a line
     * (a cut stage line or record is dropped whole); the tx tables come
     * first and are complete. */
    len=sgu_timing_show(ctx,buf);assert_whole_lines(buf,len);assert(strstr(buf,"tx submit_event n "));
    free(buf);
    /* Reset through the attribute's path. */
    assert(tbstream_gpu_timing_reset(&f.sdev)==0&&!ctx->timing[0].ops&&!ctx->timing[1].count[SGU_T_TOTAL]);
    fixture_close(&f);assert(atomic_load(&module_refs)==0);
    /* A failed operation (CRC error on RX) is not part of the timing. */
    fixture_open(&f,1,256,true);ctx=f.sdev.gpu_context;
    assert(prepare(&f,1,0,2,1,0,8192)==0);assert(action(&f,SGU_IOC_SUBMIT,1,2,1)==0);
    rx=f.sdev.rx_ring.ring;
    {
        struct ring_frame *rf=container_of(rx->queue.next,struct ring_frame,list);
        rf->flags=RING_DESC_CRC_ERROR;rf->size=0;rf->sof=1;rf->eof=2;
        list_del_init(&rf->list);rf->callback(rx,rf,false);
    }
    complete_all(rx);
    unsigned events=0;
    for(;;){struct sgu_event e={.fence=ctx->identity};
        if(tbstream_gpu_ioctl(&f.file,SGU_IOC_EVENT,(unsigned long)&e))break;
        assert(e.status==SGU_EVENT_FAILED);events+=e.count;}
    assert(events==3&&!ctx->timing[1].ops&&!ctx->timing[1].count[SGU_T_TOTAL]);
    fixture_close(&f);assert(atomic_load(&module_refs)==0);
    struct tbstream_dev none={0};static char small[PAGE_SIZE];
    assert(tbstream_gpu_timing_reset(&none)==-ENXIO&&tbstream_gpu_timing_show(&none,small)==5);
}
/* 0041: opt-in polled completion. */
/* 0042: the session's CPU latency request lives from ARM to STOP, close,
 * a failed arm or abandonment; never more than one per session. */
static void test_session_latency_qos(void) {
    struct fixture f;
    char *buf=calloc(1,PAGE_SIZE);assert(buf);
    /* Default: no request. */
    qos_live=qos_adds=0;
    fixture_open(&f,1,64,true);
    assert(f.sdev.gpu_context->qos_us==-1&&!qos_live&&!qos_adds);
    sgu_timing_show(f.sdev.gpu_context,buf);assert(strstr(buf," qos_us -1\n"));
    fixture_close(&f);
    /* The module parameter, clamped; the endpoint's value overrides it. */
    latency_qos_us=1000000;
    fixture_open(&f,1,64,true);
    assert(qos_live==1&&f.sdev.gpu_context->qos.value==100000&&f.sdev.gpu_context->qos_us==100000);
    fixture_close(&f);assert(!qos_live);
    fixture_qos_us=0;
    fixture_open(&f,1,64,true);
    struct tbstream_gpu_context *ctx=f.sdev.gpu_context;
    assert(qos_live==1&&ctx->qos.active&&ctx->qos.value==0&&ctx->qos_us==0);
    sgu_timing_show(ctx,buf);assert(strstr(buf," qos_us 0\n"));
    /* STOP releases it; a second stop and the close do nothing more. */
    assert(action(&f,SGU_IOC_STOP,0,0,0)==0);
    assert(!qos_live&&!ctx->qos.active&&ctx->qos_us==-1);
    mutex_lock(&f.sdev.lock);tbstream_gpu_stop(&f.sdev);mutex_unlock(&f.sdev.lock);
    assert(!qos_live);
    fixture_close(&f);assert(!qos_live&&qos_adds==2);
    /* Close while armed releases it. */
    fixture_open(&f,1,64,true);assert(qos_live==1);
    fixture_close(&f);assert(!qos_live);
    /* An arm that fails after starting the rings releases it. */
    fixture_open(&f,1,64,false);
    /* Not armed yet: nothing held, and the read-out says so. */
    assert(!qos_live&&f.sdev.gpu_context->qos_us==-1);
    sgu_timing_show(f.sdev.gpu_context,buf);assert(strstr(buf," qos_us -1\n"));
    {struct sgu_import req=import_request(&f,1);
     assert(tbstream_gpu_ioctl(&f.file,SGU_IOC_IMPORT,(unsigned long)&req)==0);
     struct sgu_action a={.fence=req.fence};
     enable_fail=1;
     assert(tbstream_gpu_ioctl(&f.file,SGU_IOC_ARM,(unsigned long)&a)==-EIO);
     enable_fail=0;}
    assert(!qos_live&&f.sdev.gpu_context->qos_us==-1);
    fixture_close(&f);
    /* Abandonment stops nothing, but the request goes. */
    fixture_open(&f,1,64,true);ctx=f.sdev.gpu_context;assert(qos_live==1);
    mutex_lock(&f.sdev.lock);sgu_abandon(&f.sdev);mutex_unlock(&f.sdev.lock);
    assert(ctx->abandoned&&!qos_live&&ctx->qos_us==-1);
    /* Test-only teardown of what a real abandonment leaks until reboot. */
    ctx->abandoned=false;ctx->dma_revoked=true;ctx->lifetime.pending_callbacks=0;
    fixture_close(&f);assert(!qos_live&&!f.sdev.refs&&!atomic_load(&module_refs));
    fixture_qos_us=-1;latency_qos_us=-1;free(buf);
}
static void test_polled_completion(void) {
    struct fixture f;
    fixture_gpu_poll=1;fixture_open(&f,1,256,true);fixture_gpu_poll=0;
    struct tbstream_gpu_context *ctx=f.sdev.gpu_context;
    assert(ctx->polled&&(f.sdev.tx_ring.ring->flags&RING_FLAG_NO_INTERRUPT)&&(f.sdev.rx_ring.ring->flags&RING_FLAG_NO_INTERRUPT));
    collect_requires=&ctx->dma_lock;collect_calls=0;
    /* The controller finished the TX operation; the first EVENT completes it. */
    assert(prepare(&f,0,0,1,1,0,4096)==0);assert(action(&f,SGU_IOC_SUBMIT,0,1,1)==0);
    assert(ctx->watchdog.due<=jiffies+1); /* fallback re-arm: 1 ms, not 100 */
    hw_complete_all(f.sdev.tx_ring.ring);
    assert(ctx->operations[0][0].remaining==2);
    {struct sgu_event e={.fence=ctx->identity};
     assert(tbstream_gpu_ioctl(&f.file,SGU_IOC_EVENT,(unsigned long)&e)==0);
     assert(e.direction==0&&e.handle==1&&e.status==SGU_EVENT_COMPLETE&&e.count==2);}
    assert(collect_calls==2&&ctx->polls==1&&ctx->polled_frames==2);
    assert(ctx->timing[0].ops==1&&ctx->timing[0].count[SGU_T_IRQ_WORK]==0&&ctx->timing[0].count[SGU_T_REAP]==1);
    /* Budget: one frame per ring and call; the RX operation needs three calls. */
    gpu_poll_budget=1;
    assert(prepare(&f,1,0,2,1,0,8192)==0);assert(action(&f,SGU_IOC_SUBMIT,1,2,1)==0);
    hw_complete_all(f.sdev.rx_ring.ring);
    for(int i=0;i<2;++i){struct sgu_event e={.fence=ctx->identity};
        assert(tbstream_gpu_ioctl(&f.file,SGU_IOC_EVENT,(unsigned long)&e)==-EAGAIN);}
    assert(ctx->operations[1][0].remaining==1);
    consume_event(&f,1,2,3);
    gpu_poll_budget=64;
    /* dma_lock held (enqueue, watchdog, STOP): EVENT does not wait and collects nothing. */
    assert(action(&f,SGU_IOC_RX_REPOST,1,2,1)==0);assert(action(&f,SGU_IOC_RETIRE,1,2,1)==0);
    assert(action(&f,SGU_IOC_RETIRE,0,1,1)==0);
    assert(prepare(&f,0,0,3,2,0,4096)==0);assert(action(&f,SGU_IOC_SUBMIT,0,3,2)==0);
    hw_complete_all(f.sdev.tx_ring.ring);
    struct mutex_holder h={.m=&ctx->dma_lock};pthread_t holder;
    assert(!pthread_create(&holder,NULL,hold_mutex,&h));while(!atomic_load(&h.held))usleep(100);
    int calls=collect_calls;
    {struct sgu_event e={.fence=ctx->identity};
     assert(tbstream_gpu_ioctl(&f.file,SGU_IOC_EVENT,(unsigned long)&e)==-EAGAIN);}
    assert(collect_calls==calls&&atomic_load(&h.held));
    assert(!pthread_join(holder,NULL));
    /* Nobody calls EVENT: the watchdog's fallback collects it and wakes pollers. */
    int before=wakeups;run_delayed_work(&ctx->watchdog);
    assert(collect_calls==calls+2&&wakeups>before&&ctx->operations[0][0].event_pending);
    assert(tbstream_gpu_poll(&f.file,NULL)&EPOLLIN);
    consume_event(&f,0,3,2);assert(action(&f,SGU_IOC_RETIRE,0,3,2)==0);
    /* STOP with finished but uncollected frames: canceled once, nothing collected after. */
    assert(prepare(&f,0,0,4,3,0,4096)==0);assert(action(&f,SGU_IOC_SUBMIT,0,4,3)==0);
    hw_complete_one(f.sdev.tx_ring.ring);
    assert(action(&f,SGU_IOC_STOP,0,0,0)==0);
    assert(ctx->lifetime.pending_callbacks==0&&ctx->operations[0][0].canceled);
    calls=collect_calls;
    {struct sgu_event e={.fence=ctx->identity};
     tbstream_gpu_ioctl(&f.file,SGU_IOC_EVENT,(unsigned long)&e);}
    assert(collect_calls==calls);
    collect_requires=NULL;
    char *buf=calloc(1,PAGE_SIZE);assert(buf);sgu_timing_show(ctx,buf);
    assert(strstr(buf,"mode poll polls "));free(buf);
    fixture_close(&f);assert(atomic_load(&module_refs)==0);
    /* Interrupt mode (the default) never collects. */
    fixture_open(&f,1,256,true);
    assert(!f.sdev.gpu_context->polled&&!(f.sdev.tx_ring.ring->flags&RING_FLAG_NO_INTERRUPT));
    calls=collect_calls;
    assert(prepare(&f,0,0,1,1,0,4096)==0);assert(action(&f,SGU_IOC_SUBMIT,0,1,1)==0);
    assert(f.sdev.gpu_context->watchdog.due>=jiffies+50);
    complete_all(f.sdev.tx_ring.ring);consume_event(&f,0,1,2);
    assert(collect_calls==calls);
    fixture_close(&f);assert(atomic_load(&module_refs)==0);
}
/* 0043: stall episodes, diagnostics only. A submitted operation pending with
 * no frame completed on its ring for more than gpu_stall_ms is one episode,
 * seen by the watchdog or the EVENT collection (or by the completion or
 * cancellation that ends it) and ended by the next completion; nothing about
 * traffic changes. The clock is driven by the test (clock_fake), so no
 * descheduling can move an interval across the threshold. */
static struct tbstream_gpu_context *stall_ctx;
static void stall_begin(unsigned ms) { clock_fake(true);gpu_stall_ms=ms;snapshot_calls=0;snapshot_hook=NULL; }
static void stall_end(void) { snapshot_forbids=NULL;snapshot_hook=NULL;stall_ctx=NULL;gpu_stall_ms=10;clock_fake(false); }
/* The callback of a batch reaped earlier (its stamps already set). */
static void deliver_reaped(struct tb_ring *r) {
    assert(!list_empty(&r->queue));
    struct ring_frame *rf=container_of(r->queue.next,struct ring_frame,list);
    struct sgu_hw_frame *sf=container_of(rf,struct sgu_hw_frame,frame);
    rf->size=sf->result.expected_bytes==4096?0:sf->result.expected_bytes;
    rf->sof=sf->result.expected_bytes<=256?0:1;rf->eof=2;
    list_del_init(&rf->list);rf->callback(r,rf,false);
}
static void test_stall_episode_once(void) {
    struct fixture f;stall_begin(20);fixture_open(&f,1,256,true);
    struct tbstream_gpu_context *ctx=f.sdev.gpu_context;
    struct sgu_hw_operation *op=&ctx->operations[0][0];
    struct sgu_stall *st=&ctx->stall[0];
    char *buf=calloc(1,PAGE_SIZE);assert(buf);
    snapshot_forbids=&ctx->lock;
    assert(prepare(&f,0,0,1,1,0,4096)==0);assert(action(&f,SGU_IOC_SUBMIT,0,1,1)==0);
    u64 deadline=op->deadline_ns;
    /* Within the threshold the watchdog sees nothing. */
    clock_advance_ms(19);run_delayed_work(&ctx->watchdog);
    assert(!st->episodes&&!st->open&&!snapshot_calls);
    clock_advance_ms(11);run_delayed_work(&ctx->watchdog);
    assert(st->episodes==1&&st->open&&snapshot_calls==1&&!ctx->stall[1].episodes);
    assert(st->last.sequence==1&&st->last.slot==0&&st->last.pending[0]==1&&st->last.pending[1]==0);
    assert(st->last.snapshot&&st->last.head==2&&st->last.tail==0&&!st->last.queued&&!st->last.front_completed);
    assert(st->last.start_ns==op->submitted_ns&&st->last.seen_ns-st->last.start_ns>=30*NSEC_PER_MSEC);
    /* Seen again while it lasts: still one episode, the ring read once. */
    clock_advance_ms(5);run_delayed_work(&ctx->watchdog);
    assert(st->episodes==1&&st->open&&snapshot_calls==1&&!st->total_ns);
    /* Diagnostics only: the operation, its deadline and the session are untouched. */
    assert(op->deadline_ns==deadline&&!op->error_flags&&!op->canceled&&op->phase==SGU_OP_PENDING&&op->remaining==2);
    assert(!ctx->failed&&!ctx->stopped&&!ctx->stats.timeouts&&atomic_load(&ctx->watchdog.scheduled));
    sgu_timing_show(ctx,buf);
    assert(strstr(buf,"stall threshold_ms 20\n")&&strstr(buf,"stall tx episodes 1 total_ns 0 max_ns 0 open 1\n"));
    assert(strstr(buf,"stall tx last seq 1 slot 0 state open duration_ns 350"));
    assert(strstr(buf," pending_tx 1 pending_rx 0 head 2 tail 0 queued 0 front_completed 0 collect_gap_ns -1\n"));
    assert(strstr(buf,"stall rx episodes 0 total_ns 0 max_ns 0 open 0\n")&&!strstr(buf,"stall rx last"));
    /* The next completion on the ring ends it, counted once. */
    complete_one(f.sdev.tx_ring.ring);
    assert(!st->open&&st->episodes==1&&st->total_ns==st->max_ns&&st->max_ns>=35*NSEC_PER_MSEC&&st->max_ns<36*NSEC_PER_MSEC);
    assert(st->last.end_ns>=st->last.seen_ns&&!st->last.canceled);
    complete_one(f.sdev.tx_ring.ring);consume_event(&f,0,1,2);
    assert(action(&f,SGU_IOC_RETIRE,0,1,1)==0);
    assert(st->episodes==1&&ctx->timing[0].ops==1);
    sgu_timing_show(ctx,buf);
    assert(strstr(buf,"stall tx episodes 1 total_ns 35")&&strstr(buf,"stall tx last seq 1 slot 0 state ended duration_ns 35"));
    /* After an idle gap, a new operation is measured from its SUBMIT. */
    clock_advance_ms(100);
    assert(prepare(&f,0,0,2,2,0,4096)==0);assert(action(&f,SGU_IOC_SUBMIT,0,2,2)==0);
    clock_advance_ms(5);run_delayed_work(&ctx->watchdog);complete_all(f.sdev.tx_ring.ring);consume_event(&f,0,2,2);
    assert(action(&f,SGU_IOC_RETIRE,0,2,2)==0);
    assert(st->episodes==1&&snapshot_calls==1);
    /* Nobody looked: the completion that ends it counts it, without a ring read. */
    assert(prepare(&f,0,0,3,3,0,4096)==0);assert(action(&f,SGU_IOC_SUBMIT,0,3,3)==0);
    clock_advance_ms(30);complete_one(f.sdev.tx_ring.ring);
    assert(st->episodes==2&&!st->open&&!st->last.snapshot&&st->last.seen_ns==st->last.end_ns&&snapshot_calls==1);
    assert(st->last.sequence==3&&st->last.end_ns-st->last.start_ns>=30*NSEC_PER_MSEC);
    sgu_timing_show(ctx,buf);
    assert(strstr(buf,"stall tx last seq 3 slot 0 state ended ")&&strstr(buf," head -1 tail -1 queued -1 front_completed -1 collect_gap_ns -1\n"));
    complete_one(f.sdev.tx_ring.ring);consume_event(&f,0,3,2);assert(st->episodes==2);
    /* A heavy but realistic session (10^6 operations, 150 ms maxima, five
     * busy buckets, full stall records) fits whole in a page. */
    for(unsigned d=0;d<2;++d){struct sgu_timing *t=&ctx->timing[d];struct sgu_stall *x=&ctx->stall[d];
        t->ops=1000000;t->next=SGU_T_RECORDS;
        for(unsigned s=0;s<SGU_T_STAGES;++s){t->count[s]=1000000;t->sum_ns[s]=50000ULL*1000000;t->max_ns[s]=150000000;
            for(unsigned b=0;b<SGU_T_BUCKETS;++b)t->hist[s][b]=b>=5&&b<10?200000:37;}
        for(unsigned i=0;i<SGU_T_RECORDS;++i){t->last[i].sequence=1000000;t->last[i].slot=7;t->last[i].submit_ns=1;
            for(unsigned s=0;s<SGU_T_STAGES;++s)t->last[i].at[s]=150000001;}
        x->episodes=12345;x->total_ns=1900000000;x->max_ns=170000000;x->open=false;
        x->last=(struct sgu_stall_record){.sequence=1000000,.slot=7,.start_ns=1,.seen_ns=150000001,.end_ns=170000001,
            .pending={8,8},.head=4095,.tail=4000,.queued=100,.snapshot=true,.canceled=true};}
    ssize_t heavy=sgu_timing_show(ctx,buf);
    assert_whole_lines(buf,heavy);assert(heavy<(ssize_t)PAGE_SIZE-512);
    {const char *last=strstr(buf,"rx last seq 1000000 slot 7 ns");unsigned records=0;
     for(;last;last=strstr(last+1,"rx last seq 1000000 slot 7 ns"))
         ++records;
     assert(records==SGU_T_RECORDS);}
    assert(strstr(buf,"stall rx last seq 1000000 slot 7 state canceled duration_ns 170000000 seen_ns 150000000 pending_tx 8 pending_rx 8 head 4095 tail 4000 queued 100 front_completed 0 collect_gap_ns -1\n"));
    /* Filled to the maximum: whole lines only, the stall lines all there. */
    for(unsigned d=0;d<2;++d){ctx->timing[d].ops=~0ULL>>1;ctx->timing[d].next=SGU_T_RECORDS;
        ctx->stall[d].episodes=ctx->stall[d].total_ns=ctx->stall[d].max_ns=~0ULL>>1;ctx->stall[d].last.sequence=~0ULL;
        for(unsigned s=0;s<SGU_T_STAGES;++s){ctx->timing[d].count[s]=~0ULL>>1;ctx->timing[d].max_ns[s]=~0ULL>>1;
            for(unsigned b=0;b<SGU_T_BUCKETS;++b)ctx->timing[d].hist[s][b]=0xffffffffU;}}
    ssize_t len=sgu_timing_show(ctx,buf);
    assert_whole_lines(buf,len);
    assert(strstr(buf,"stall rx last seq 18446744073709551615 ")&&strstr(buf," collect_gap_ns -1\ntx ops "));
    /* Reset clears the counts. */
    assert(tbstream_gpu_timing_reset(&f.sdev)==0&&!ctx->stall[0].episodes&&!ctx->stall[1].max_ns&&!ctx->stall[0].last.sequence);
    free(buf);
    fixture_close(&f);assert(atomic_load(&module_refs)==0);stall_end();
}
/* Reset keeps the progress: an operation that keeps completing frames is
 * not a stall afterwards; an open episode is counted again once seen. */
static void test_stall_reset_keeps_progress(void) {
    struct fixture f;stall_begin(60);fixture_open(&f,1,256,true);
    struct tbstream_gpu_context *ctx=f.sdev.gpu_context;
    struct sgu_stall *st=&ctx->stall[0];
    assert(prepare(&f,0,0,1,1,0,3*4096)==0);assert(action(&f,SGU_IOC_SUBMIT,0,1,1)==0);
    clock_advance_ms(40);complete_one(f.sdev.tx_ring.ring);
    assert(!st->episodes);
    assert(tbstream_gpu_timing_reset(&f.sdev)==0);
    clock_advance_ms(30);run_delayed_work(&ctx->watchdog); /* 30 ms since the frame, 70 since SUBMIT */
    assert(!st->episodes&&!st->open);
    clock_advance_ms(40);run_delayed_work(&ctx->watchdog);
    assert(st->episodes==1&&st->open);
    u64 start=st->last.start_ns;
    assert(start==ctx->progress_ns[0]&&start>ctx->operations[0][0].submitted_ns);
    assert(tbstream_gpu_timing_reset(&f.sdev)==0&&!st->episodes&&!st->open);
    run_delayed_work(&ctx->watchdog);
    assert(st->episodes==1&&st->open&&st->last.start_ns==start);
    complete_all(f.sdev.tx_ring.ring);
    assert(st->episodes==1&&!st->open&&st->max_ns>=70*NSEC_PER_MSEC&&st->max_ns<71*NSEC_PER_MSEC);
    consume_event(&f,0,1,4);
    fixture_close(&f);assert(atomic_load(&module_refs)==0);stall_end();
}
/* Polled mode, nobody calls EVENT: the watchdog's collection sees and ends
 * the episode. The interval between collections separates "nobody looked"
 * from "no progress", also while an episode is open and when it is
 * canceled; a held DMA lock is no collection; the EVENT collection is one. */
static void test_stall_polled_without_event(void) {
    struct fixture f;stall_begin(20);
    fixture_gpu_poll=1;fixture_open(&f,1,256,true);fixture_gpu_poll=0;
    struct tbstream_gpu_context *ctx=f.sdev.gpu_context;
    struct sgu_stall *st=&ctx->stall[0];
    char *buf=calloc(1,PAGE_SIZE);assert(buf);
    snapshot_forbids=&ctx->lock;
    assert(prepare(&f,0,0,1,1,0,4096)==0);assert(action(&f,SGU_IOC_SUBMIT,0,1,1)==0);
    clock_advance_ms(30);run_delayed_work(&ctx->watchdog);
    assert(st->episodes==1&&st->open&&st->last.snapshot&&st->last.head==2&&!st->last.front_completed);
    assert(ctx->collect_gap_ns[0]>=30*NSEC_PER_MSEC); /* nobody looked for 30 ms */
    sgu_timing_show(ctx,buf);assert(strstr(buf,"mode poll ")&&strstr(buf," collect_gap_ns 30"));
    hw_complete_all(f.sdev.tx_ring.ring);
    assert(st->open); /* finished, not collected */
    clock_advance_ms(1);run_delayed_work(&ctx->watchdog);
    assert(!st->open&&st->episodes==1&&st->last.collect_gap_ns>=30*NSEC_PER_MSEC&&ctx->operations[0][0].event_pending);
    consume_event(&f,0,1,2);assert(action(&f,SGU_IOC_RETIRE,0,1,1)==0);
    /* Collected every millisecond: no progress, and somebody looked. */
    assert(prepare(&f,0,0,2,2,0,4096)==0);assert(action(&f,SGU_IOC_SUBMIT,0,2,2)==0);
    for(int i=0;i<25;++i){clock_advance_ms(1);run_delayed_work(&ctx->watchdog);}
    assert(st->episodes==2&&st->open&&st->last.sequence==2);
    assert(ctx->collect_gap_ns[0]>=NSEC_PER_MSEC&&ctx->collect_gap_ns[0]<2*NSEC_PER_MSEC);
    sgu_timing_show(ctx,buf);assert(strstr(buf,"state open ")&&strstr(buf," collect_gap_ns 100"));
    /* Then the collections stop: the open episode shows the time since the last one. */
    clock_advance_ms(100);
    sgu_timing_show(ctx,buf);
    {const char *g=strstr(buf,"stall tx last ");assert(g);g=strstr(g," collect_gap_ns ");assert(g);
     assert(strtoull(g+16,NULL,10)>=100*NSEC_PER_MSEC);}
    assert(ctx->collect_gap_ns[0]<2*NSEC_PER_MSEC); /* shown, not stored */
    hw_complete_one(f.sdev.tx_ring.ring);run_delayed_work(&ctx->watchdog);
    assert(!st->open&&st->episodes==2&&st->last.collect_gap_ns>=100*NSEC_PER_MSEC);
    hw_complete_all(f.sdev.tx_ring.ring);consume_event(&f,0,2,2);assert(action(&f,SGU_IOC_RETIRE,0,2,2)==0);
    /* EVENT that cannot take the DMA lock collects nothing and sees nothing. */
    assert(prepare(&f,0,0,3,3,0,4096)==0);assert(action(&f,SGU_IOC_SUBMIT,0,3,3)==0);
    clock_advance_ms(30);
    struct mutex_holder h={.m=&ctx->dma_lock};pthread_t holder;
    assert(!pthread_create(&holder,NULL,hold_mutex,&h));while(!atomic_load(&h.held))usleep(100);
    u64 collected=ctx->collected_ns;
    {struct sgu_event e={.fence=ctx->identity};
     assert(tbstream_gpu_ioctl(&f.file,SGU_IOC_EVENT,(unsigned long)&e)==-EAGAIN);}
    assert(st->episodes==2&&ctx->collected_ns==collected);
    assert(!pthread_join(holder,NULL));
    clock_advance_ms(20);
    {struct sgu_event e={.fence=ctx->identity};
     assert(tbstream_gpu_ioctl(&f.file,SGU_IOC_EVENT,(unsigned long)&e)==-EAGAIN);}
    assert(st->episodes==3&&st->open&&ctx->collected_ns>collected&&ctx->collect_gap_ns[0]>=50*NSEC_PER_MSEC);
    hw_complete_all(f.sdev.tx_ring.ring);consume_event(&f,0,3,2);
    assert(!st->open&&st->episodes==3&&st->last.collect_gap_ns>=50*NSEC_PER_MSEC);
    assert(action(&f,SGU_IOC_RETIRE,0,3,3)==0);
    /* Canceled after the collections stopped: the final interval counts. */
    assert(prepare(&f,0,0,4,4,0,4096)==0);assert(action(&f,SGU_IOC_SUBMIT,0,4,4)==0);
    for(int i=0;i<25;++i){clock_advance_ms(1);run_delayed_work(&ctx->watchdog);}
    assert(st->episodes==4&&st->open&&ctx->collect_gap_ns[0]<2*NSEC_PER_MSEC);
    clock_advance_ms(80);
    assert(action(&f,SGU_IOC_STOP,0,0,0)==0);
    assert(!st->open&&st->last.canceled&&st->episodes==4&&st->last.collect_gap_ns>=80*NSEC_PER_MSEC);
    assert(ctx->lifetime.pending_callbacks==0);
    free(buf);
    fixture_close(&f);assert(atomic_load(&module_refs)==0);stall_end();
}
/* RX is detected on its own; a standby RX is not pending; a reused RX is
 * measured from its SUBMIT; STOP cancels once and ends the episode. */
static void test_stall_rx_standby_and_stop(void) {
    struct fixture f;stall_begin(20);fixture_open(&f,1,256,true);
    struct tbstream_gpu_context *ctx=f.sdev.gpu_context;
    struct sgu_stall *st=&ctx->stall[1];
    char *buf=calloc(1,PAGE_SIZE);assert(buf);
    assert(prepare(&f,1,0,2,1,0,8192)==0);assert(action(&f,SGU_IOC_SUBMIT,1,2,1)==0);
    complete_all(f.sdev.rx_ring.ring);consume_event(&f,1,2,3);
    assert(action(&f,SGU_IOC_RX_REPOST,1,2,1)==0);assert(action(&f,SGU_IOC_RETIRE,1,2,1)==0);
    clock_advance_ms(30);run_delayed_work(&ctx->watchdog);
    assert(!st->episodes&&!ctx->stall[0].episodes); /* posted, not expected */
    assert(prepare(&f,1,0,3,2,0,8191)==0);assert(action(&f,SGU_IOC_SUBMIT,1,3,2)==0);
    run_delayed_work(&ctx->watchdog);assert(!st->episodes);
    clock_advance_ms(30);run_delayed_work(&ctx->watchdog);
    assert(st->episodes==1&&st->open&&!ctx->stall[0].episodes);
    assert(st->last.start_ns==ctx->operations[1][0].submitted_ns&&st->last.sequence==2);
    assert(st->last.pending[0]==0&&st->last.pending[1]==1&&st->last.head==3);
    /* STOP: every frame canceled once, the episode ends canceled. */
    clock_advance_ms(5);
    assert(action(&f,SGU_IOC_STOP,0,0,0)==0);
    assert(ctx->lifetime.pending_callbacks==0&&ctx->operations[1][0].canceled);
    assert(!st->open&&st->last.canceled&&st->episodes==1&&st->max_ns>=35*NSEC_PER_MSEC);
    sgu_timing_show(ctx,buf);assert(strstr(buf,"stall rx last seq 2 slot 0 state canceled duration_ns "));
    free(buf);
    fixture_close(&f);assert(atomic_load(&module_refs)==0);stall_end();
}
/* A cancellation ends an interval no collection point saw (IRQ mode, STOP
 * before the 100 ms watchdog): counted once as canceled, not erased. A
 * shorter one is no episode, and a standby RX canceled with it is none. */
static void test_stall_cancel_unseen(void) {
    struct fixture f;stall_begin(20);fixture_open(&f,1,256,true);
    struct tbstream_gpu_context *ctx=f.sdev.gpu_context;
    assert(prepare(&f,1,0,2,1,0,8192)==0);assert(action(&f,SGU_IOC_SUBMIT,1,2,1)==0);
    complete_all(f.sdev.rx_ring.ring);consume_event(&f,1,2,3);
    assert(action(&f,SGU_IOC_RX_REPOST,1,2,1)==0);assert(action(&f,SGU_IOC_RETIRE,1,2,1)==0);
    assert(prepare(&f,0,0,3,1,0,2*4096)==0);assert(action(&f,SGU_IOC_SUBMIT,0,3,1)==0);
    clock_advance_ms(50);
    assert(action(&f,SGU_IOC_STOP,0,0,0)==0);
    struct sgu_stall *st=&ctx->stall[0];
    assert(ctx->lifetime.pending_callbacks==0&&!snapshot_calls);
    assert(st->episodes==1&&!st->open&&st->last.canceled&&!st->last.snapshot&&st->last.sequence==1);
    assert(st->max_ns>=50*NSEC_PER_MSEC&&st->max_ns<51*NSEC_PER_MSEC&&st->last.seen_ns==st->last.end_ns);
    assert(!ctx->stall[1].episodes); /* the standby RX was canceled too */
    fixture_close(&f);assert(atomic_load(&module_refs)==0);
    fixture_open(&f,1,256,true);ctx=f.sdev.gpu_context;
    assert(prepare(&f,0,0,1,1,0,4096)==0);assert(action(&f,SGU_IOC_SUBMIT,0,1,1)==0);
    clock_advance_ms(10);
    assert(action(&f,SGU_IOC_STOP,0,0,0)==0);
    assert(!ctx->stall[0].episodes&&!ctx->stall[1].episodes);
    fixture_close(&f);assert(atomic_load(&module_refs)==0);stall_end();
}
/* Explicit orderings of the races. A collection point sees the episode
 * after a batch was reaped but before its callbacks ran: it ends at the
 * later sighting. The episode ends, or the counts are reset, between the
 * ring read and its store: the read is dropped. */
static void stall_hook_complete(struct tb_ring *r) { snapshot_hook=NULL;complete_one(r); }
static void stall_hook_reset(struct tb_ring *r) { (void)r;snapshot_hook=NULL;sgu_timing_reset(stall_ctx); }
static void test_stall_races(void) {
    struct fixture f;stall_begin(20);fixture_open(&f,1,256,true);
    struct tbstream_gpu_context *ctx=f.sdev.gpu_context;
    struct sgu_stall *st=&ctx->stall[0];
    struct tb_ring *tx=f.sdev.tx_ring.ring;
    snapshot_forbids=&ctx->lock;stall_ctx=ctx;
    /* Reaped at 30 ms, seen at 35 ms, callback afterwards. */
    assert(prepare(&f,0,0,1,1,0,4096)==0);assert(action(&f,SGU_IOC_SUBMIT,0,1,1)==0);
    u64 submit=ctx->operations[0][0].submitted_ns;
    clock_advance_ms(30);
    tx->cb_stamps.irq_ns=ktime_get_ns();tx->cb_stamps.start_ns=ktime_get_ns();tx->cb_stamps.reap_ns=ktime_get_ns();
    clock_advance_ms(5);run_delayed_work(&ctx->watchdog);
    assert(st->episodes==1&&st->open);
    u64 seen=st->last.seen_ns;
    deliver_reaped(tx);
    assert(!st->open&&st->episodes==1&&st->last.end_ns==seen&&st->max_ns==seen-submit);
    assert(ctx->progress_ns[0]==tx->cb_stamps.reap_ns&&ctx->progress_ns[0]<seen);
    complete_one(tx);consume_event(&f,0,1,2);assert(action(&f,SGU_IOC_RETIRE,0,1,1)==0);
    assert(st->episodes==1);
    /* Ended between the ring read and its store. */
    assert(prepare(&f,0,0,2,2,0,4096)==0);assert(action(&f,SGU_IOC_SUBMIT,0,2,2)==0);
    clock_advance_ms(30);snapshot_hook=stall_hook_complete;run_delayed_work(&ctx->watchdog);
    assert(st->episodes==2&&!st->open&&!st->last.snapshot&&snapshot_calls==2&&!snapshot_hook);
    complete_one(tx);consume_event(&f,0,2,2);assert(action(&f,SGU_IOC_RETIRE,0,2,2)==0);
    /* Reset between the ring read and its store; the next look counts it again. */
    assert(prepare(&f,0,0,3,3,0,4096)==0);assert(action(&f,SGU_IOC_SUBMIT,0,3,3)==0);
    clock_advance_ms(30);snapshot_hook=stall_hook_reset;run_delayed_work(&ctx->watchdog);
    assert(!st->episodes&&!st->open&&!st->last.snapshot&&!st->last.seen_ns&&snapshot_calls==3);
    clock_advance_ms(1);run_delayed_work(&ctx->watchdog);
    assert(st->episodes==1&&st->open&&st->last.snapshot&&snapshot_calls==4);
    complete_all(tx);consume_event(&f,0,3,2);
    assert(st->episodes==1&&!st->open);
    fixture_close(&f);assert(atomic_load(&module_refs)==0);stall_end();
}
/* gpu_stall_ms 0: no episode, no ring read, no collection bookkeeping; an
 * episode already open still ends at its completion. */
static void test_stall_threshold_zero(void) {
    struct fixture f;stall_begin(0);fixture_open(&f,1,256,true);
    struct tbstream_gpu_context *ctx=f.sdev.gpu_context;
    char *buf=calloc(1,PAGE_SIZE);assert(buf);
    assert(prepare(&f,0,0,1,1,0,4096)==0);assert(action(&f,SGU_IOC_SUBMIT,0,1,1)==0);
    assert(prepare(&f,1,0,2,1,0,8192)==0);assert(action(&f,SGU_IOC_SUBMIT,1,2,1)==0);
    clock_advance_ms(30);run_delayed_work(&ctx->watchdog);
    complete_all(f.sdev.tx_ring.ring);complete_all(f.sdev.rx_ring.ring);
    assert(!ctx->stall[0].episodes&&!ctx->stall[1].episodes&&!snapshot_calls);
    consume_event(&f,0,1,2);consume_event(&f,1,2,3);
    sgu_timing_show(ctx,buf);
    assert(strstr(buf,"stall threshold_ms 0\nstall tx episodes 0 total_ns 0 max_ns 0 open 0\nstall rx episodes 0 "));
    assert(action(&f,SGU_IOC_RETIRE,0,1,1)==0);
    gpu_stall_ms=20;
    assert(prepare(&f,0,0,3,2,0,4096)==0);assert(action(&f,SGU_IOC_SUBMIT,0,3,2)==0);
    clock_advance_ms(30);run_delayed_work(&ctx->watchdog);
    assert(ctx->stall[0].open&&ctx->stall[0].episodes==1&&snapshot_calls==1);
    snapshot_calls=0;gpu_stall_ms=0;complete_one(f.sdev.tx_ring.ring);
    assert(!ctx->stall[0].open&&ctx->stall[0].episodes==1&&ctx->stall[0].max_ns>=30*NSEC_PER_MSEC);
    complete_one(f.sdev.tx_ring.ring);consume_event(&f,0,3,2);
    /* Off, a cancellation counts nothing either. */
    assert(action(&f,SGU_IOC_RETIRE,0,3,2)==0);
    assert(prepare(&f,0,0,4,3,0,4096)==0);assert(action(&f,SGU_IOC_SUBMIT,0,4,3)==0);
    clock_advance_ms(30);assert(action(&f,SGU_IOC_STOP,0,0,0)==0);
    assert(ctx->stall[0].episodes==1&&!ctx->stall[0].open&&!snapshot_calls);
    fixture_close(&f);assert(atomic_load(&module_refs)==0);
    /* Polled: no collection bookkeeping while off. */
    fixture_gpu_poll=1;fixture_open(&f,1,256,true);fixture_gpu_poll=0;ctx=f.sdev.gpu_context;
    assert(prepare(&f,0,0,1,1,0,4096)==0);assert(action(&f,SGU_IOC_SUBMIT,0,1,1)==0);
    clock_advance_ms(30);run_delayed_work(&ctx->watchdog);
    assert(!ctx->collected_ns&&!ctx->collect_gap_ns[0]&&!ctx->stall[0].episodes&&ctx->polls==1);
    hw_complete_all(f.sdev.tx_ring.ring);consume_event(&f,0,1,2);
    assert(!ctx->stall[0].episodes);
    free(buf);
    fixture_close(&f);assert(atomic_load(&module_refs)==0);stall_end();
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
#include "sgu_detached_cases.h"

#include "sgu_group_cases.h"
#include "sgu_teardown_cases.h"
#include "sgu_terminal_cases.h"

int main(int argc,char **argv) {
    assert(argc==2);
    struct { const char *name; void (*run)(void); } tests[]={
        {"geometry",test_geometry},{"queue",test_queue},{"failure_isolation",test_failure_isolation},
        {"events_legacy",test_events_and_legacy},{"timeout_recovery",test_timeout_recovery},
        {"wait_concurrency",test_wait_concurrency},{"nonblock_contended_mutex",test_nonblock_contended_mutex},{"nonblock_contended_dma_lock",test_nonblock_contended_dma_lock},{"completion_timing",test_completion_timing},{"polled_completion",test_polled_completion},{"session_latency_qos",test_session_latency_qos},
        {"stall_episode_once",test_stall_episode_once},{"stall_reset_keeps_progress",test_stall_reset_keeps_progress},
        {"stall_polled_without_event",test_stall_polled_without_event},{"stall_rx_standby_and_stop",test_stall_rx_standby_and_stop},
        {"stall_threshold_zero",test_stall_threshold_zero},
        {"stall_cancel_unseen",test_stall_cancel_unseen},{"stall_races",test_stall_races},{"wait_retry_order",test_wait_retry_and_order},
        {"export_lifetime",test_export_lifetime},{"admission_rollback",test_admission_and_rollback},
        {"watchdog_schedule",test_watchdog_schedule},{"max_queue_reuse",test_max_queue_reuse},
        {"group_success",test_group_success},{"group_rejections",test_group_rejections},
        {"group_copy_failure",test_group_copy_failure},
        {"domain_retirement_open_fds",test_domain_retirement_open_fds},
        {"domain_retirement_keeps_unproved_dma",test_domain_retirement_keeps_unproved_dma},
        {"domain_retirement_foreign_domain",test_domain_retirement_foreign_domain},
        {"domain_retirement_concurrent_poll",test_domain_retirement_concurrent_poll},
        {"domain_revoked_releases_retained",test_domain_revoked_releases_retained},
        {"domain_abandoned_leaks_safely",test_domain_abandoned_leaks_safely},
        {"orphan_reaping_after_rmdir",test_orphan_reaping_after_rmdir},
        {"terminal_header",test_terminal_header},
        {"terminal_mismatches",test_terminal_mismatches},
        {"terminal_standby_reuse",test_terminal_standby_reuse},
        {"watchdog_mutex_independence",test_watchdog_mutex_independence},
        {"fence_precedes_event",test_fence_precedes_event},
        {"stop_before_publication",test_stop_before_publication},
        {"quarantine_reaping",test_quarantine_reaping},
        {"explicit_controller_reset",test_explicit_controller_reset},
        {"statistics_snapshot",test_statistics_snapshot},
        {"detached_reap",test_detached_reap},
        {"detached_path_proof",test_detached_path_proof},
        {"detached_device_guard",test_detached_device_guard},
        {"detached_reset_rejected",test_detached_reset_rejected}};
    for(unsigned i=0;i<sizeof(tests)/sizeof(tests[0]);++i)if(!strcmp(tests[i].name,argv[1])) {
        tests[i].run();printf("PASS %s\n",argv[1]);return 0;
    }
    return 2;
}
