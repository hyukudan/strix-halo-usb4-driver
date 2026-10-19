/* SPDX-License-Identifier: GPL-2.0-only */
/* Runs the actual nhi_reset() and nhi_reset_interface() of nhi.c against a
 * logged MMIO model. The register offsets and the quirk bit are taken from
 * the patched nhi_regs.h and nhi.h by the test driver. */

static struct { u32 offset, value; } writes[8];
static int nwrites, caps_reads_after_write;
static u32 caps_value;
static long slept_us;
static int infos, warns;
static bool hrr_pending;

#define MMIO_BASE ((char *)0x10000000)
static u32 reg_offset(const void *addr) { return (u32)((const char *)addr - MMIO_BASE); }
static u32 ioread32(const void *addr)
{
    u32 offset = reg_offset(addr);

    if (offset == REG_CAPS) {
        if (nwrites)
            ++caps_reads_after_write;
        return caps_value;
    }
    if (offset == REG_RESET) {
        bool pending = hrr_pending;

        hrr_pending = false; /* the reset completes after the first poll */
        return pending ? REG_RESET_HRR : 0;
    }
    return 0;
}
static void iowrite32(u32 value, void *addr)
{
    assert(nwrites < 8);
    writes[nwrites].offset = reg_offset(addr);
    writes[nwrites++].value = value;
    if (reg_offset(addr) == REG_RESET && (value & REG_RESET_HRR))
        hrr_pending = true;
}
static void usleep_range(unsigned long min, unsigned long max) { (void)max; slept_us += min; }
static void msleep(unsigned int ms) { slept_us += ms * 1000L; }
static ktime_t ktime_get(void) { return slept_us * 1000; }
#define ktime_add_ms(t, ms) ((t) + (ktime_t)(ms) * 1000000)
#define ktime_before(a, b) ((a) < (b))
#define dev_info(dev, ...) ((void)(dev), ++infos)
#define dev_warn(dev, ...) ((void)(dev), ++warns)
#define dev_dbg(dev, ...) ((void)(dev))

static void reset_model(u32 caps)
{
    memset(writes, 0, sizeof(writes));
    nwrites = caps_reads_after_write = infos = warns = 0;
    slept_us = 0;
    hrr_pending = false;
    caps_value = caps;
}

#define CAPS_V1 3U                    /* what the Strix Halo NHIs report */
#define CAPS_V2 ((0x40U << 16) | 21U)

/* v1 AMD NHI (REG_CAPS = 3): HRR does not exist, the host interface reset
 * runs instead, flushed and followed by tHIReset. */
static void v1_quirked_interface_reset(void)
{
    struct device dev;
    struct tb_nhi nhi = { .iobase = MMIO_BASE, .dev = &dev, .quirks = QUIRK_RESET_INTERFACE_ON_PROBE };

    reset_model(CAPS_V1);
    host_reset = true;
    nhi_reset(&nhi);
    assert(nwrites == 1);
    assert(writes[0].offset == REG_HOST_INTERFACE_RESET && writes[0].value == REG_HOST_INTERFACE_RESET_RST);
    assert(caps_reads_after_write >= 1 && slept_us >= 10000 && infos == 1);
}

/* Other v1 NHIs, or host_reset=N: nothing is written. */
static void v1_untouched(void)
{
    struct device dev;
    struct tb_nhi plain = { .iobase = MMIO_BASE, .dev = &dev };
    struct tb_nhi quirked = { .iobase = MMIO_BASE, .dev = &dev, .quirks = QUIRK_RESET_INTERFACE_ON_PROBE };

    reset_model(CAPS_V1);
    host_reset = true;
    nhi_reset(&plain);
    assert(nwrites == 0 && !infos);
    reset_model(CAPS_V1);
    host_reset = false;
    nhi_reset(&quirked);
    assert(nwrites == 0 && !infos);
    host_reset = true;
}

/* v2 routers still get HRR, never the v1 interface reset. */
static void v2_router_reset(void)
{
    struct device dev;
    struct tb_nhi nhi = { .iobase = MMIO_BASE, .dev = &dev, .quirks = QUIRK_RESET_INTERFACE_ON_PROBE };

    reset_model(CAPS_V2);
    host_reset = true;
    nhi_reset(&nhi);
    assert(nwrites == 1 && writes[0].offset == REG_RESET && writes[0].value == REG_RESET_HRR);
    assert(warns == 1); /* "host router reset successful" */
    reset_model(CAPS_V2);
    host_reset = false;
    nhi_reset(&nhi);
    assert(nwrites == 0);
    host_reset = true;
}

int main(int argc, char **argv)
{
    struct { const char *name; void (*run)(void); } cases[] = {
        { "v1_quirked_interface_reset", v1_quirked_interface_reset },
        { "v1_untouched", v1_untouched },
        { "v2_router_reset", v2_router_reset },
    };

    assert(argc == 2);
    for (unsigned int i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        if (!strcmp(cases[i].name, argv[1])) {
            cases[i].run();
            printf("PASS %s\n", argv[1]);
            return 0;
        }
    }
    return 2;
}
