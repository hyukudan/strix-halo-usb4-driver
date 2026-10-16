/* SPDX-License-Identifier: GPL-2.0-only */
struct recovery_fixture {
    struct pci_dev pci;
    struct tb tb;
    struct tb_nhi nhi;
    struct tb_ctl ctl;
    struct tb_ring tx0,rx0,tx,rx,other;
};
static void ring_init(struct tb_ring *r,struct tb_nhi *nhi,unsigned hop,bool tx) {
    memset(r,0,sizeof(*r));mutex_init(&r->lock);r->nhi=nhi;r->hop=hop;r->is_tx=tx;
    INIT_LIST_HEAD(&r->queue);INIT_LIST_HEAD(&r->in_flight);
}
static void recovery_init(struct recovery_fixture *f) {
    memset(f,0,sizeof(*f));mutex_init(&f->nhi.lock);mutex_init(&f->tb.lock);
    f->pci.vendor=PCI_VENDOR_ID_AMD;f->pci.device=PCI_DEVICE_ID_AMD_1AH_M70H_NHI0;
    f->pci.present=true;f->pci.dev.pci=true;f->pci.dev.pdev=&f->pci;
    f->tb.nhi=&f->nhi;f->tb.ctl=&f->ctl;f->tb.dev.registered=true;
    f->ctl.nhi=&f->nhi;f->nhi.dev=&f->pci.dev;f->nhi.hop_count=8;f->nhi.iobase=bar;
    ring_init(&f->tx0,&f->nhi,0,true);ring_init(&f->rx0,&f->nhi,0,false);
    ring_init(&f->tx,&f->nhi,2,true);ring_init(&f->rx,&f->nhi,3,false);
    ring_init(&f->other,&f->nhi,4,true);
    f->nhi.tx_rings[0]=&f->tx0;f->nhi.rx_rings[0]=&f->rx0;
    f->nhi.tx_rings[2]=&f->tx;f->nhi.rx_rings[3]=&f->rx;
    f->tx0.running=f->rx0.running=true;
    caps=0x200008; /* NHI v1, eight hops */
}
static void test_admission(void) {
    struct recovery_fixture f;recovery_init(&f);
    assert(nhi_dma_reset_begin(&f.nhi,&f.tx,&f.rx)==0);
    assert(f.nhi.dma_resetting);
    assert(nhi_dma_reset_begin(&f.nhi,&f.tx,&f.rx)==-EBUSY);
    nhi_dma_reset_end(&f.nhi);
    f.nhi.tx_rings[4]=&f.other;
    assert(nhi_dma_reset_begin(&f.nhi,&f.tx,&f.rx)==-EBUSY); /* idle sibling still reserved */
    f.nhi.tx_rings[4]=NULL;f.tx.running=true;
    assert(nhi_dma_reset_begin(&f.nhi,&f.tx,&f.rx)==-EBUSY);
    f.tx.running=false;f.tx.queue.next=&f.other.queue;
    assert(nhi_dma_reset_begin(&f.nhi,&f.tx,&f.rx)==-EBUSY);
    INIT_LIST_HEAD(&f.tx.queue);f.rx.in_flight.next=&f.other.queue;
    assert(nhi_dma_reset_begin(&f.nhi,&f.tx,&f.rx)==-EBUSY);
    INIT_LIST_HEAD(&f.rx.in_flight);
    assert(nhi_dma_reset_begin(&f.nhi,&f.rx,&f.tx)==-EINVAL);
    assert(nhi_dma_reset_begin(&f.nhi,&f.tx0,&f.rx)==-EINVAL);
    f.nhi.going_away=true;
    assert(nhi_dma_reset_begin(&f.nhi,&f.tx,&f.rx)==-ENODEV);
    f.nhi.going_away=false;f.nhi.dma_shutdown=true;
    assert(nhi_dma_reset_begin(&f.nhi,&f.tx,&f.rx)==-ENODEV);
}
static void test_reset_success(void) {
    struct recovery_fixture f;recovery_init(&f);
    assert(tb_domain_reset_dma(&f.tb,&f.tx,&f.rx)==0);
    assert(writes==1&&stops==1&&starts==1&&pm_refs==0&&!f.nhi.dma_resetting);
    assert(f.tx0.running&&f.rx0.running&&!f.tx.running&&!f.rx.running);
}
static void test_unsupported(void) {
    struct recovery_fixture f;recovery_init(&f);
    f.pci.vendor=0x8086;
    assert(tb_domain_reset_dma(&f.tb,&f.tx,&f.rx)==-EOPNOTSUPP);
    f.pci.vendor=PCI_VENDOR_ID_AMD;f.pci.device=999;
    assert(tb_domain_reset_dma(&f.tb,&f.tx,&f.rx)==-EOPNOTSUPP);
    f.pci.device=PCI_DEVICE_ID_AMD_1AH_M70H_NHI0;caps=0x400008;
    assert(tb_domain_reset_dma(&f.tb,&f.tx,&f.rx)==-EOPNOTSUPP);
    caps=~0U;
    assert(tb_domain_reset_dma(&f.tb,&f.tx,&f.rx)==-ENODEV);
    caps=0x200008;f.pci.present=false;
    assert(tb_domain_reset_dma(&f.tb,&f.tx,&f.rx)==-ENODEV);
    f.pci.present=true;f.nhi.going_away=true;int before=reads;
    assert(tb_domain_reset_dma(&f.tb,&f.tx,&f.rx)==-ENODEV&&reads==before);
    f.nhi.going_away=false;f.nhi.tx_rings[4]=&f.other;
    assert(tb_domain_reset_dma(&f.tb,&f.tx,&f.rx)==-EBUSY);
    assert(!writes&&!starts&&!stops&&!pm_refs);
    f.nhi.tx_rings[4]=NULL;pm_error=-ENOMEM;
    assert(tb_domain_reset_dma(&f.tb,&f.tx,&f.rx)==-ENOMEM&&!pm_refs);
}
static void test_reset_failure(void) {
    struct recovery_fixture f;recovery_init(&f);broken_readback=true;
    assert(tb_domain_reset_dma(&f.tb,&f.tx,&f.rx)==-EIO);
    assert(writes==1&&stops==1&&!starts&&!pm_refs&&f.nhi.dma_resetting);
    assert(!f.tx0.running&&!f.rx0.running);
    broken_readback=false;
    assert(tb_domain_reset_dma(&f.tb,&f.tx,&f.rx)==-EBUSY&&writes==1);
}
static void test_control_failure(void) {
    struct recovery_fixture f;recovery_init(&f);broken_control=true;
    assert(tb_domain_reset_dma(&f.tb,&f.tx,&f.rx)==-EIO);
    assert(writes==1&&stops==2&&starts==1&&!pm_refs&&f.nhi.dma_resetting);
}
int main(int argc,char **argv) {
    assert(argc==2);
    if(!strcmp(argv[1],"admission"))test_admission();
    else if(!strcmp(argv[1],"reset_success"))test_reset_success();
    else if(!strcmp(argv[1],"unsupported"))test_unsupported();
    else if(!strcmp(argv[1],"reset_failure"))test_reset_failure();
    else if(!strcmp(argv[1],"control_failure"))test_control_failure();
    else return 2;
    printf("PASS %s\n",argv[1]);return 0;
}
