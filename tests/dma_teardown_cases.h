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
    f->pci.dev.drvdata = &f->tb;
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
    assert(infos == 1 && !warns && !errs);
    assert(strstr(last_info, "pending: NHI in use (1 TX and 1 RX rings allocated besides control"));
    assert(f.nhi.teardown_reset_pending && f.nhi.teardown_reset_parked);
    /* An idle, merely allocated ring of another user (a quarantined GPU
     * context, a closed-but-not-freed ring) blocks it too. */
    teardown_init(&f);
    f.nhi.tx_rings[2] = NULL;
    tb_domain_schedule_teardown_reset(&f.tb);
    assert(run_attempts(&f) == 10 && writes == 0);
    assert(strstr(last_info, "(0 TX and 1 RX rings"));
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
    assert(strstr(last_info, "pending: domain lock busy (0 TX and 0 RX rings"));
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
    assert(run_attempts(&f) == 10 && !writes && infos == 1 && !warns);
    assert(strstr(last_info, "pending: NHI in use (0 TX and 0 RX rings"));
    /* The tunnel's own teardown re-arms it. */
    inventory_result = 0;
    tb_domain_schedule_teardown_reset(&f.tb);
    assert(run_attempts(&f) == 1 && writes == 1 && !f.nhi.teardown_reset_pending);
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

/* teardown_init() with the global counters of the harness at zero. */
static void teardown_fresh(struct recovery_fixture *f)
{
    teardown_init(f);
    writes = stops = starts = infos = warns = errs = 0;
    nqueued = cancels = 0;
}

/* Frees a ring the way tb_ring_free() does: out of its slot, then the hook. */
static void free_ring(struct recovery_fixture *f, bool tx, unsigned hop)
{
    if (tx)
        f->nhi.tx_rings[hop] = NULL;
    else
        f->nhi.rx_rings[hop] = NULL;
    tb_nhi_ring_freed(&f->nhi);
}

/* Parks the reset behind the fixture's other stream (hops 2/3). */
static void park_behind_other_stream(struct recovery_fixture *f)
{
    teardown_fresh(f);
    f->tx.running = f->rx.running = true;
    tb_domain_schedule_teardown_reset(&f->tb);   /* stream A closes */
    assert(run_attempts(f) == 10 && !writes);
    assert(f->nhi.teardown_reset_pending && f->nhi.teardown_reset_parked);
    assert(!f->tb.teardown_reset_work.pending && infos == 1);
    assert(strstr(last_info, "pending: NHI in use"));
}

/* Audit A12/F: A closes while B stays open longer than the fast attempts;
 * then B closes. The reset was dropped before; now it is pending, parked
 * without polling, and runs once B is gone, whichever of B's events comes
 * last: its path teardown or the free of its last ring. */
static void test_teardown_reset_pending_until_owner_leaves(void)
{
    struct recovery_fixture f;
    unsigned queued;

    /* B closes normally: paths first, rings right after. */
    park_behind_other_stream(&f);
    queued = nqueued;
    tb_domain_schedule_teardown_reset(&f.tb);
    assert(f.tb.teardown_reset_attempts == 0 && nqueued == queued + 1);
    f.tx.running = f.rx.running = false;
    free_ring(&f, true, 2);
    free_ring(&f, false, 3);
    assert(run_attempts(&f) == 1 && writes == 1);
    assert(strstr(last_info, "host interface reset after DMA teardown (attempt 1)"));
    assert(!f.nhi.teardown_reset_pending && !f.nhi.teardown_reset_parked);

    /* B's paths went without a reset owed (or were never there): only its
     * rings are freed, late, the last one re-arms it. Parked, nothing is
     * polled meanwhile. */
    park_behind_other_stream(&f);
    queued = nqueued;
    f.tx.running = f.rx.running = false;
    free_ring(&f, true, 2);                      /* RX ring still allocated */
    assert(!f.tb.teardown_reset_work.pending && nqueued == queued && f.nhi.teardown_reset_parked);
    free_ring(&f, false, 3);                     /* the last data ring */
    assert(f.tb.teardown_reset_work.pending && queued_delays[nqueued - 1] == 100);
    assert(!f.nhi.teardown_reset_parked && f.tb.teardown_reset_attempts == 0);
    assert(run_attempts(&f) == 1 && writes == 1 && !f.nhi.teardown_reset_pending);
    assert(infos == 2 && !warns && !errs);

    /* Nothing pending: freeing rings schedules nothing. */
    teardown_fresh(&f);
    free_ring(&f, true, 2);
    free_ring(&f, false, 3);
    assert(!f.tb.teardown_reset_work.pending && !nqueued);
}

/* The exclusivity is checked again right before the reset: a consumer that
 * appears between the re-arm and the attempt keeps it pending. */
static void test_teardown_reset_rechecks_before_reset(void)
{
    struct recovery_fixture f;

    park_behind_other_stream(&f);
    f.tx.running = f.rx.running = false;
    free_ring(&f, true, 2);
    free_ring(&f, false, 3);
    assert(f.tb.teardown_reset_work.pending);
    f.nhi.tx_rings[4] = &f.other;                   /* a new stream opens */
    assert(run_attempts(&f) == 10 && !writes);
    assert(f.nhi.teardown_reset_pending && f.nhi.teardown_reset_parked);
    assert(infos == 2 && strstr(last_info, "pending: NHI in use (1 TX and 0 RX rings"));
    free_ring(&f, true, 4);
    assert(run_attempts(&f) == 1 && writes == 1);
}

/* Domain removal with a reset pending: dropped and logged, never re-armed
 * by a later ring free, and the NHI is not touched. */
static void test_teardown_reset_pending_at_removal(void)
{
    struct recovery_fixture f;

    park_behind_other_stream(&f);
    f.nhi.dma_shutdown = true;                   /* tb_domain_remove() */
    /* Its service drivers free their rings before the work is stopped. */
    f.tx.running = f.rx.running = false;
    free_ring(&f, true, 2);
    free_ring(&f, false, 3);
    assert(!f.tb.teardown_reset_work.pending && f.nhi.teardown_reset_pending);
    tb_domain_teardown_reset_stop(&f.tb, true);
    assert(cancels == 1 && !f.nhi.teardown_reset_pending && !f.nhi.teardown_reset_parked);
    assert(infos == 2 && strstr(last_info, "dropped: the domain is being removed"));
    assert(!f.tb.teardown_reset_work.pending && !writes);
    tb_domain_teardown_reset_resume(&f.tb);
    assert(!f.tb.teardown_reset_work.pending);
    /* Removal while attempts are queued: cancelled, dropped. */
    teardown_fresh(&f);
    tb_domain_schedule_teardown_reset(&f.tb);
    assert(f.tb.teardown_reset_work.pending);
    f.nhi.dma_shutdown = true;
    tb_domain_teardown_reset_stop(&f.tb, true);
    assert(!f.tb.teardown_reset_work.pending && !f.nhi.teardown_reset_pending && !writes);
    /* Nothing pending: no log. */
    teardown_fresh(&f);
    tb_domain_teardown_reset_stop(&f.tb, true);
    assert(!infos);
}

/* Suspend stops the attempts but keeps the reset pending; resume re-arms
 * it with a fresh budget. */
static void test_teardown_reset_pending_across_suspend(void)
{
    struct recovery_fixture f;

    teardown_fresh(&f);
    tb_domain_schedule_teardown_reset(&f.tb);
    assert(run_delayed(&f.tb.teardown_reset_work) && !writes);
    tb_domain_teardown_reset_stop(&f.tb, false);   /* tb_domain_suspend() */
    assert(!f.tb.teardown_reset_work.pending && f.nhi.teardown_reset_pending);
    free_stream_rings(&f);
    tb_domain_teardown_reset_resume(&f.tb);        /* tb_domain_complete() */
    assert(f.tb.teardown_reset_work.pending && f.tb.teardown_reset_attempts == 0);
    assert(run_attempts(&f) == 1 && writes == 1 && strstr(last_info, "(attempt 1)"));
    tb_domain_teardown_reset_resume(&f.tb);
    assert(!f.tb.teardown_reset_work.pending);
}
