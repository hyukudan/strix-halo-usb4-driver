/* SPDX-License-Identifier: GPL-2.0-only */
/* Included by dma_recovery_cases.h. The automatic host interface reset after
 * a DMA path teardown (0018): only on an idle quirked v1 NHI, with a bounded
 * number of attempts, never waiting for the domain lock, and a log line for
 * what happened. recovery_init() leaves the rings f.tx (hop 2) and f.rx
 * (hop 3) allocated: "another stream" on the same NHI unless a case frees them. */

static void teardown_init(struct recovery_fixture *f)
{
    static struct workqueue_struct wq;

    recovery_init(f);
    f->tb.wq = &wq;
    f->tb.teardown_reset_work.func = tb_domain_teardown_reset_work;
}

static void free_stream_rings(struct recovery_fixture *f)
{
    f->nhi.tx_rings[2] = NULL;
    f->nhi.rx_rings[3] = NULL;
}

/* Run every queued attempt; returns how many ran. */
static unsigned run_attempts(struct recovery_fixture *f)
{
    unsigned n = 0;

    while (run_delayed(&f->tb.teardown_reset_work))
        ++n;
    return n;
}

/* The stream that was torn down was the NHI's only user: one reset, the
 * control channel restarted, a dmesg line. */
static void test_teardown_reset_idle(void)
{
    struct recovery_fixture f;

    teardown_init(&f);
    free_stream_rings(&f);
    tb_domain_schedule_teardown_reset(&f.tb);
    assert(f.tb.teardown_reset_work.pending && queued_delays[0] == 100);
    assert(run_attempts(&f) == 1);
    assert(writes == 1 && stops == 1 && starts == 1 && !pm_refs && !f.nhi.dma_resetting);
    assert(f.tx0.running && f.rx0.running);
    assert(infos == 1 && strstr(last_info, "host interface reset after DMA teardown (attempt 1)"));
    assert(!warns && !errs);
}

/* Another stream still has its rings on the same NHI: never reset; after the
 * bounded attempts, one warning with the state. */
static void test_teardown_reset_other_stream(void)
{
    struct recovery_fixture f;

    teardown_init(&f);
    f.tx.running = f.rx.running = true;
    tb_domain_schedule_teardown_reset(&f.tb);
    assert(run_attempts(&f) == 10);
    assert(!writes && !stops && !starts && !pm_refs && !f.nhi.dma_resetting);
    assert(f.tx.running && f.rx.running && f.tx0.running && f.rx0.running);
    assert(nqueued == 10 && queued_delays[0] == 100 && queued_delays[9] == 500);
    assert(warns == 1 && !infos && !errs);
    assert(strstr(last_warn, "NHI in use after 10 attempts (1 TX and 1 RX rings allocated besides control"));
    /* An idle, merely allocated ring of another user (a quarantined GPU
     * context, a closed-but-not-freed ring) blocks it too. */
    teardown_init(&f);
    f.nhi.tx_rings[2] = NULL;
    tb_domain_schedule_teardown_reset(&f.tb);
    assert(run_attempts(&f) == 10 && writes == 0);
    assert(strstr(last_warn, "(0 TX and 1 RX rings"));
}

/* The owner frees its rings just after disabling its paths: the first
 * attempt finds them, the next one resets. */
static void test_teardown_reset_waits_for_owner(void)
{
    struct recovery_fixture f;

    teardown_init(&f);
    tb_domain_schedule_teardown_reset(&f.tb);
    assert(run_delayed(&f.tb.teardown_reset_work) && !writes);
    free_stream_rings(&f);
    assert(run_attempts(&f) == 1 && writes == 1);
    assert(strstr(last_info, "(attempt 2)") && !warns);
}

/* The CM holds the domain lock: the work does not wait for it. */
static void test_teardown_reset_lock_busy(void)
{
    struct recovery_fixture f;

    teardown_init(&f);
    free_stream_rings(&f);
    mutex_lock(&f.tb.lock);
    tb_domain_schedule_teardown_reset(&f.tb);
    assert(run_delayed(&f.tb.teardown_reset_work) && run_delayed(&f.tb.teardown_reset_work));
    assert(!writes && !pm_refs);
    mutex_unlock(&f.tb.lock);
    assert(run_attempts(&f) == 1 && writes == 1 && strstr(last_info, "(attempt 3)"));
    /* Held for the whole budget: bounded, then reported. */
    teardown_init(&f);
    free_stream_rings(&f);
    mutex_lock(&f.tb.lock);
    tb_domain_schedule_teardown_reset(&f.tb);
    assert(run_attempts(&f) == 10 && writes == 1);
    assert(strstr(last_warn, "domain lock busy after 10 attempts (0 TX and 0 RX rings"));
    mutex_unlock(&f.tb.lock);
}

/* A DMA tunnel still in the CM inventory (another XDomain's stream) vetoes
 * it, even with no ring allocated. */
static void test_teardown_reset_tunnel_veto(void)
{
    struct recovery_fixture f;

    teardown_init(&f);
    free_stream_rings(&f);
    inventory_result = 1;
    tb_domain_schedule_teardown_reset(&f.tb);
    assert(run_attempts(&f) == 10 && !writes && warns == 1);
    inventory_result = 0;
}

/* Never scheduled on other NHIs, when disabled, or once the domain is going
 * away; v2 routers are refused by the reset itself, without retries. */
static void test_teardown_reset_gated(void)
{
    struct recovery_fixture f;

    teardown_init(&f);
    free_stream_rings(&f);
    f.pci.vendor = 0x8086;
    tb_domain_schedule_teardown_reset(&f.tb);
    assert(!f.tb.teardown_reset_work.pending);
    f.pci.vendor = PCI_VENDOR_ID_AMD;
    teardown_reset = false;
    tb_domain_schedule_teardown_reset(&f.tb);
    assert(!f.tb.teardown_reset_work.pending);
    teardown_reset = true;
    f.nhi.dma_shutdown = true;
    tb_domain_schedule_teardown_reset(&f.tb);
    assert(!f.tb.teardown_reset_work.pending);
    f.nhi.dma_shutdown = false;
    f.nhi.going_away = true;
    tb_domain_schedule_teardown_reset(&f.tb);
    assert(!f.tb.teardown_reset_work.pending);
    f.nhi.going_away = false;
    caps = 0x400008;
    tb_domain_schedule_teardown_reset(&f.tb);
    assert(run_attempts(&f) == 1 && !writes && !warns && !errs && !infos);
    /* Domain removal starting after it was queued: one -ENODEV, no retry. */
    caps = 0x200008;
    tb_domain_schedule_teardown_reset(&f.tb);
    f.nhi.dma_shutdown = true;
    assert(run_attempts(&f) == 1 && !writes && !warns);
}

/* The reset itself fails: logged as an error, admission stays closed (the
 * series' rule: hardware state not trusted until reprobe), no retry. */
static void test_teardown_reset_failure(void)
{
    struct recovery_fixture f;

    teardown_init(&f);
    free_stream_rings(&f);
    broken_readback = true;
    tb_domain_schedule_teardown_reset(&f.tb);
    assert(run_attempts(&f) == 1 && writes == 1 && f.nhi.dma_resetting);
    assert(errs == 1 && strstr(last_err, "failed: -5"));
    broken_readback = false;
}

/* A new teardown restarts the budget. */
static void test_teardown_reset_budget_restarts(void)
{
    struct recovery_fixture f;

    teardown_init(&f);
    tb_domain_schedule_teardown_reset(&f.tb);
    for (int i = 0; i < 9; ++i)
        assert(run_delayed(&f.tb.teardown_reset_work));
    tb_domain_schedule_teardown_reset(&f.tb); /* the other stream closes too */
    assert(f.tb.teardown_reset_attempts == 0 && queued_delays[nqueued - 1] == 100);
    free_stream_rings(&f);
    assert(run_attempts(&f) == 1 && writes == 1 && strstr(last_info, "(attempt 1)") && !warns);
}
