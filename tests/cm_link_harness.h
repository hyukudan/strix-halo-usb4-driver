/* SPDX-License-Identifier: GPL-2.0-only */
/* Minimal kernel model for the link handling of the connection manager:
 * lane bonding of the lower-UUID XDomain side (xdomain.c), the host router
 * reset at start (tb.c) and the opt-in Gen3 retrain (xdomain.c). The
 * functions under test are the actual ones, extracted from the sources;
 * registers, sideband and resets are scripted models. */
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef uint8_t u8;
typedef uint32_t u32;
#define BIT(n) (1U << (n))
#define GENMASK(h, l) (((~0U) << (l)) & (~0U >> (31 - (h))))
#define FIELD_GET(mask, v) (((v) & (mask)) >> __builtin_ctz(mask))
#define READ_ONCE(x) (x)
#define min(a, b) ((a) < (b) ? (a) : (b))
#define min_t(t, a, b) ((t)(a) < (t)(b) ? (t)(a) : (t)(b))

/* Upstream register layout (tb_regs.h, sb_regs.h) */
enum tb_cfg_space { TB_CFG_HOPS, TB_CFG_PORT, TB_CFG_SWITCH };
#define LANE_ADP_CS_0				0x00
#define LANE_ADP_CS_0_SUPPORTED_SPEED_MASK	GENMASK(19, 16)
#define LANE_ADP_CS_1				0x01
#define LANE_ADP_CS_1_TARGET_SPEED_MASK		GENMASK(3, 0)
#define LANE_ADP_CS_1_TARGET_SPEED_GEN3		0xc
#define USB4_SB_LINK_CONF			0x0c
enum usb4_sb_target { USB4_SB_TARGET_ROUTER, USB4_SB_TARGET_PARTNER, USB4_SB_TARGET_RETIMER };

struct mutex { pthread_mutex_t m; };
static bool mutex_trylock(struct mutex *m) { return !pthread_mutex_trylock(&m->m); }
static void mutex_unlock(struct mutex *m) { assert(!pthread_mutex_unlock(&m->m)); }
static bool mutex_is_free(struct mutex *m)
{
    if (pthread_mutex_trylock(&m->m))
        return false;
    pthread_mutex_unlock(&m->m);
    return true;
}
#define lockdep_assert_held(m) assert(!mutex_is_free(m))

/* Log lines */
static int infos, warns;
static char last_info[256], last_warn[256];
#define dev_info(d, ...) ((void)(d), ++infos, snprintf(last_info, sizeof(last_info), __VA_ARGS__))
#define dev_warn(d, ...) ((void)(d), ++warns, snprintf(last_warn, sizeof(last_warn), __VA_ARGS__))
#define tb_warn(tb, ...) ((void)(tb), ++warns, snprintf(last_warn, sizeof(last_warn), __VA_ARGS__))

struct device { int unused; };
struct tb_port { int cap_usb4, cap_phy; unsigned int gen3_retrains; };
struct tb_nhi { int unused; };
struct tb;
struct tb_cm_ops { int (*tunnels_active)(struct tb *tb); };
struct tb_switch { bool usb4; unsigned int version; };
struct tb {
    struct mutex lock;
    const struct tb_cm_ops *cm_ops;
    struct tb_nhi *nhi;
    struct tb_switch *root_switch;
};
struct tb_xdomain { struct device dev; struct tb *tb; int state_retries; struct tb_port *port; };

/* E1: scripted results of tb_xdomain_lane_bonding_enable() */
static int bonding_results[8], bonding_calls;
static int tb_xdomain_lane_bonding_enable(struct tb_xdomain *xd)
{
    (void)xd;
    assert(bonding_calls < 8);
    return bonding_results[bonding_calls++];
}

/* E2 */
static int switch_reset_ret, switch_resets;
static bool tb_switch_is_usb4(const struct tb_switch *sw) { return sw->usb4; }
static unsigned int usb4_switch_version(const struct tb_switch *sw) { return sw->version; }
static int tb_switch_reset(struct tb_switch *sw) { (void)sw; ++switch_resets; return switch_reset_ret; }

/* E3: the local port as the registers report it. The domain lock of the
 * fixture (link_tb) serializes the sideband mailbox, the tunnel inventory
 * and the reset; the mocks check it is held. */
static unsigned int gen3_retrain;
static int link_gen = 2, port_read_ret, sb_read_ret, port_reset_ret, port_resets, tunnels;
static int gen_reads, port_reads, sb_reads;
static u32 cs0 = 0xc << 16, cs1 = 0xc, link_conf = 0xf300;
static bool rings_idle = true, exclusive;
static struct tb *link_tb;
static struct tb_port *tb_xdomain_downstream_port(struct tb_xdomain *xd) { return xd->port; }
static int tb_port_get_link_generation(struct tb_port *port)
{
    (void)port;
    ++gen_reads;
    return link_gen;
}
static int tb_port_read(struct tb_port *port, void *buf, enum tb_cfg_space space, u32 offset, u32 length)
{
    ++port_reads;
    assert(space == TB_CFG_PORT && length == 1);
    if (port_read_ret)
        return port_read_ret;
    if (offset == (u32)port->cap_phy + LANE_ADP_CS_0)
        *(u32 *)buf = cs0;
    else if (offset == (u32)port->cap_phy + LANE_ADP_CS_1)
        *(u32 *)buf = cs1;
    else
        assert(0);
    return 0;
}
static int usb4_port_sb_read(struct tb_port *port, enum usb4_sb_target target, u8 index, u8 reg, void *buf, u8 size)
{
    (void)port;
    ++sb_reads;
    assert(!mutex_is_free(&link_tb->lock)); /* the shared mailbox, as debugfs */
    assert(target == USB4_SB_TARGET_ROUTER && !index && reg == USB4_SB_LINK_CONF && size == 3);
    if (sb_read_ret)
        return sb_read_ret;
    memcpy(buf, &link_conf, 3);
    return 0;
}
/* Ring allocation/start, as nhi.c gates it: refused while the NHI is held
 * exclusive (dma_resetting). Another user trying it during the reset. */
static bool ring_alloc_allowed(void) { return !exclusive; }
static int usb4_port_reset(struct tb_port *port)
{
    (void)port;
    ++port_resets;
    assert(!mutex_is_free(&link_tb->lock));
    assert(!ring_alloc_allowed()); /* no ring can appear until the reset is done */
    return port_reset_ret;
}
static int tb_nhi_dma_exclusive_begin(struct tb_nhi *nhi)
{
    (void)nhi;
    assert(!exclusive);
    if (!rings_idle)
        return -EBUSY;
    exclusive = true;
    return 0;
}
static void tb_nhi_dma_exclusive_end(struct tb_nhi *nhi)
{
    (void)nhi;
    assert(exclusive);
    exclusive = false;
}
static int cm_tunnels_active(struct tb *tb)
{
    assert(!mutex_is_free(&tb->lock)); /* the CM inventory, under the domain lock */
    return tunnels;
}
static const struct tb_cm_ops cm_ops = { .tunnels_active = cm_tunnels_active };
