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

int main(int argc, char **argv)
{
    assert(argc == 2);
    if (!strcmp(argv[1], "bonding_retried_and_bounded"))
        bonding_retried_and_bounded();
    else
        return 2;
    printf("PASS %s\n", argv[1]);
    return 0;
}
