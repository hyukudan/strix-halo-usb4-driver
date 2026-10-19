/* SPDX-License-Identifier: GPL-2.0-only */
/* Included after the extracted functions. Audit E1-E3. */

static void reset_model(void)
{
    infos = warns = 0;
    last_info[0] = last_warn[0] = 0;
    bonding_calls = switch_resets = port_resets = 0;
    memset(bonding_results, 0, sizeof(bonding_results));
    switch_reset_ret = port_read_ret = sb_read_ret = port_reset_ret = paths_busy = 0;
    link_gen = 2;
    cs0 = 0xc << 16;    /* supports Gen2 and Gen3 */
    cs1 = 0xc;          /* targets Gen3 */
    link_conf = 0xf300; /* byte 1 = f3: Gen3 Support set */
    rings_idle = true;
    gen3_retrain = 0;
}

/* E1: lane bonding on the lower-UUID side was attempted once and its
 * result ignored. Now: retried a bounded number of times, then reported;
 * a missing second lane adapter is not retried. */
static void bonding_retried_and_bounded(void)
{
    struct tb_xdomain xd = { .state_retries = XDOMAIN_BONDING_RETRIES };

    reset_model();
    assert(!tb_xdomain_bond_lanes_uuid_low(&xd) && bonding_calls == 1 && !infos);

    /* Lane 1 late: two failures, then bonded. */
    reset_model();
    xd.state_retries = XDOMAIN_BONDING_RETRIES;
    bonding_results[0] = -ETIMEDOUT;
    bonding_results[1] = -ENOTCONN;
    assert(tb_xdomain_bond_lanes_uuid_low(&xd) == -EAGAIN);
    assert(infos == 1 && strstr(last_info, "lane bonding failed (-110), retrying"));
    assert(tb_xdomain_bond_lanes_uuid_low(&xd) == -EAGAIN);
    assert(!tb_xdomain_bond_lanes_uuid_low(&xd) && bonding_calls == 3);

    /* Never bonds: XDOMAIN_BONDING_RETRIES + 1 attempts, then the error. */
    reset_model();
    xd.state_retries = XDOMAIN_BONDING_RETRIES;
    for (int i = 0; i < 8; ++i)
        bonding_results[i] = -ETIMEDOUT;
    for (int i = 0; i < XDOMAIN_BONDING_RETRIES; ++i)
        assert(tb_xdomain_bond_lanes_uuid_low(&xd) == -EAGAIN);
    assert(tb_xdomain_bond_lanes_uuid_low(&xd) == -ETIMEDOUT);
    assert(bonding_calls == XDOMAIN_BONDING_RETRIES + 1);

    /* An -EAGAIN from below never loops the state machine. */
    reset_model();
    xd.state_retries = 0;
    bonding_results[0] = -EAGAIN;
    assert(tb_xdomain_bond_lanes_uuid_low(&xd) == -ETIMEDOUT);

    /* No dual-link port: final at once. */
    reset_model();
    xd.state_retries = XDOMAIN_BONDING_RETRIES;
    bonding_results[0] = -ENODEV;
    assert(tb_xdomain_bond_lanes_uuid_low(&xd) == -ENODEV && bonding_calls == 1 && !infos);
}

/* E2: tb_start() ignored the result of the host router reset and went on
 * as if the boot firmware's tunnels were gone. */
static void host_reset_result_checked(void)
{
    struct tb_switch sw = { .usb4 = true, .version = 1 };
    struct tb tb = { .root_switch = &sw };

    reset_model();
    assert(!tb_start_reset_host(&tb, true) && switch_resets == 1 && !warns);

    reset_model();
    switch_reset_ret = -EIO;
    assert(tb_start_reset_host(&tb, true) && switch_resets == 1);
    assert(warns == 1 && strstr(last_warn, "host router reset failed (-5), discovering the boot firmware configuration instead"));

    /* Unchanged cases: no reset requested, USB4 v2 (reset elsewhere), TBT. */
    reset_model();
    assert(tb_start_reset_host(&tb, false) && !switch_resets);
    sw.version = 2;
    assert(!tb_start_reset_host(&tb, true) && !switch_resets);
    sw.usb4 = false;
    assert(tb_start_reset_host(&tb, true) && !switch_resets && !warns);
}

static void link_fixture(struct tb *tb, struct tb_nhi *nhi, struct tb_port *port, struct tb_xdomain *xd)
{
    memset(tb, 0, sizeof(*tb));
    assert(!pthread_mutex_init(&tb->lock.m, NULL));
    tb->cm_ops = &cm_ops;
    tb->nhi = nhi;
    *port = (struct tb_port){ .cap_usb4 = 0x30, .cap_phy = 0x10 };
    *xd = (struct tb_xdomain){ .tb = tb, .port = port };
}

/* E3: opt-in, bounded, logged retrain of a new link that came up at Gen2
 * while both the lane adapter and the local port advertise Gen3. */
static void gen3_retrain_policy(void)
{
    struct tb tb;
    struct tb_nhi nhi;
    struct tb_port port;
    struct tb_xdomain xd;

    /* Off by default: nothing read, nothing reset. */
    reset_model();
    link_fixture(&tb, &nhi, &port, &xd);
    assert(!tb_xdomain_maybe_retrain_gen3(&xd) && !port_resets && !infos && !warns);

    /* On, everything advertises Gen3: one reset, the handshake restarts. */
    gen3_retrain = 2;
    assert(tb_xdomain_maybe_retrain_gen3(&xd) == -ECONNRESET);
    assert(port_resets == 1 && port.gen3_retrains == 1 && mutex_is_free(&tb.lock));
    assert(strstr(last_info, "port reset, retrain 1 of 2"));

    /* The local port without Gen3 Support (byte 1 = d3, the field case):
     * the limit is the port or its retimers, logged, never retrained. */
    reset_model();
    link_fixture(&tb, &nhi, &port, &xd);
    gen3_retrain = 2;
    link_conf = 0xd300;
    assert(!tb_xdomain_maybe_retrain_gen3(&xd) && !port_resets && !port.gen3_retrains);
    assert(strstr(last_info, "does not advertise Gen3 Support (LINK_CONF 0x00d300)"));
    link_conf = 0xf300;
    sb_read_ret = -EIO;
    assert(!tb_xdomain_maybe_retrain_gen3(&xd) && !port_resets && strstr(last_info, "LINK_CONF unreadable (-5)"));
    sb_read_ret = 0;

    /* Lane adapter without Gen3, or a CM that targets Gen2 only. */
    cs0 = 0x8 << 16;
    assert(!tb_xdomain_maybe_retrain_gen3(&xd) && !port_resets && strstr(last_info, "does not support or target Gen3"));
    cs0 = 0xc << 16;
    cs1 = 0x8;
    assert(!tb_xdomain_maybe_retrain_gen3(&xd) && !port_resets);
    cs1 = 0xc;

    /* Never with traffic: a DMA tunnel, a data ring, or the lock taken. */
    paths_busy = 1;
    assert(!tb_xdomain_maybe_retrain_gen3(&xd) && !port_resets && strstr(last_info, "in use, not retraining"));
    paths_busy = 0;
    rings_idle = false;
    assert(!tb_xdomain_maybe_retrain_gen3(&xd) && !port_resets);
    rings_idle = true;
    assert(!pthread_mutex_lock(&tb.lock.m));
    assert(!tb_xdomain_maybe_retrain_gen3(&xd) && !port_resets && strstr(last_info, "domain lock busy"));
    mutex_unlock(&tb.lock);
    assert(!port.gen3_retrains && mutex_is_free(&tb.lock));

    /* Bounded: two resets, then one warning, then silence. */
    assert(tb_xdomain_maybe_retrain_gen3(&xd) == -ECONNRESET);
    assert(tb_xdomain_maybe_retrain_gen3(&xd) == -ECONNRESET && port_resets == 2);
    assert(!tb_xdomain_maybe_retrain_gen3(&xd) && port_resets == 2);
    assert(warns == 1 && strstr(last_warn, "still at Gen2 after 2 Gen3 retrain(s)"));
    assert(!tb_xdomain_maybe_retrain_gen3(&xd) && port_resets == 2 && warns == 1);
    /* The link reaches Gen3: logged, the budget starts again. */
    link_gen = 3;
    assert(!tb_xdomain_maybe_retrain_gen3(&xd) && !port.gen3_retrains);
    assert(strstr(last_info, "trained at Gen3 after 2 Gen3 retrain(s)"));
    assert(!tb_xdomain_maybe_retrain_gen3(&xd) && infos && port_resets == 2);

    /* A failed reset is reported and the enumeration goes on. */
    link_gen = 2;
    port_reset_ret = -EIO;
    assert(!tb_xdomain_maybe_retrain_gen3(&xd) && port_resets == 3);
    assert(strstr(last_warn, "port reset for a Gen3 retrain failed (-5)") && mutex_is_free(&tb.lock));

    /* The parameter is capped. */
    reset_model();
    link_fixture(&tb, &nhi, &port, &xd);
    gen3_retrain = 100;
    for (int i = 0; i < 10; ++i)
        tb_xdomain_maybe_retrain_gen3(&xd);
    assert(port_resets == XDOMAIN_GEN3_RETRAIN_MAX);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    if (!strcmp(argv[1], "bonding_retried_and_bounded"))
        bonding_retried_and_bounded();
    else if (!strcmp(argv[1], "host_reset_result_checked"))
        host_reset_result_checked();
    else if (!strcmp(argv[1], "gen3_retrain_policy"))
        gen3_retrain_policy();
    else
        return 2;
    printf("PASS %s\n", argv[1]);
    return 0;
}
