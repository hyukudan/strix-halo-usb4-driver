/* SPDX-License-Identifier: GPL-2.0-only */
/* Runs the actual tb_xdomain_presence() of xdomain.c against a bus model:
 * named devices, of which some are XDomains, with reference counting. */
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef unsigned long long u64; /* as in the kernel */
#define READ_ONCE(x) (x)
#define EXPORT_SYMBOL_GPL(x)

struct device { const char *name; bool xdomain; int refs; };
struct tb_xdomain { struct device dev; bool is_unplugged; };
struct bus_type { int unused; };
static const struct bus_type tb_bus_type;
static struct tb_xdomain bus[8];
static int nbus;

static struct device *bus_find_device_by_name(const struct bus_type *b, struct device *start, const char *name)
{
    assert(b == &tb_bus_type && !start);
    for (int i = 0; i < nbus; ++i) {
        if (!strcmp(bus[i].dev.name, name)) {
            bus[i].dev.refs++;
            return &bus[i].dev;
        }
    }
    return NULL;
}
static void put_device(struct device *dev) { assert(dev->refs-- > 0); }
static bool tb_is_xdomain(const struct device *dev) { return dev->xdomain; }
static struct tb_xdomain *tb_to_xdomain(struct device *dev)
{
    return dev->xdomain ? (struct tb_xdomain *)((char *)dev - __builtin_offsetof(struct tb_xdomain, dev)) : NULL;
}

static int tb_xdomain_presence(unsigned int domain, u64 route);

static void add(const char *name, bool xdomain, bool unplugged)
{
    assert(nbus < 8);
    bus[nbus].dev.name = name;
    bus[nbus].dev.xdomain = xdomain;
    bus[nbus++].is_unplugged = unplugged;
}

static void presence(void)
{
    /* The 27-minute case: endpoints of domain 1, which no longer exists. */
    add("domain0", false, false);
    add("0-2", true, false);
    assert(tb_xdomain_presence(1, 2) == -ENODEV);
    assert(tb_xdomain_presence(0, 2) == 0);
    /* Domain there, no XDomain at that route (no cable or no peer). */
    assert(tb_xdomain_presence(0, 1) == -ENXIO);
    /* A router with the same name format is not an XDomain. */
    add("domain1", false, false);
    add("1-2", false, false);
    assert(tb_xdomain_presence(1, 2) == -ENXIO);
    /* Being unplugged: it cannot come back. */
    bus[1].is_unplugged = true;
    assert(tb_xdomain_presence(0, 2) == -ENXIO);
    /* Route printed in hex, as in the device names. */
    add("domain2", false, false);
    add("2-30001", true, false);
    assert(tb_xdomain_presence(2, 0x30001) == 0);
    for (int i = 0; i < nbus; ++i)
        assert(bus[i].dev.refs == 0);
}

int main(int argc, char **argv)
{
    assert(argc == 2 && !strcmp(argv[1], "presence"));
    presence();
    printf("PASS presence\n");
    return 0;
}
