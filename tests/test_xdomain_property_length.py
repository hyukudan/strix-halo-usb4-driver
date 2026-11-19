#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Issue #7: exercise the patched XDomain property response length arithmetic.

Extract the *actual* request-side guard from reconstructed xdomain.c so the
test fails if the stable patch is omitted. The kernel build in CI separately
checks the whole C translation unit.
"""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = Path(os.environ.get("USB4_SOURCE_TREE", ROOT / "audited-overlay"))
PATCH = "0049-xdomain-property-chunk-length.patch"


class XDomainPropertyLengthTests(unittest.TestCase):
    def test_series_includes_fix(self):
        for filename in ("series", "audit-series"):
            names = (ROOT / "patches" / filename).read_text().splitlines()
            self.assertIn(PATCH, names)
        self.assertIn("res->hdr.xd_hdr", (ROOT / "patches" / PATCH).read_text())

    def test_actual_kernel_guard_accepts_final_one_or_two_dwords(self):
        source = (SOURCE / "drivers/thunderbolt/xdomain.c").read_text()
        anchor = "len = res->hdr.xd_hdr.length_sn & TB_XDOMAIN_LENGTH_MASK;"
        self.assertEqual(source.count(anchor), 1)
        start = source.index(anchor)
        end = source.index("\n\n\t\tif (res->offset != req.offset)", start)
        guard = source[start:end]
        self.assertIn("goto err;", guard)
        self.assertIn("len -= sizeof(*res) / 4;", guard)
        # Copy this exact source fragment into a small C harness. The response
        # has 12 bytes of XDomain header and 60 of fixed payload (72 total).
        c = r"""
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#define TB_XDOMAIN_LENGTH_MASK 0x03ffU
struct xdomain_hdr { uint16_t length_sn; uint8_t rest[10]; };
struct response {
    struct { struct xdomain_hdr xd_hdr; } hdr;
    uint8_t fixed_payload[60];
};
_Static_assert(sizeof(struct response) == 72, "response fixture size");
_Static_assert(sizeof(struct xdomain_hdr) == 12, "XDomain header size");

static int data_dwords(uint16_t field)
{
    struct response storage = {0};
    struct response *res = &storage;
    uint16_t len;
    int ret = 0;
    res->hdr.xd_hdr.length_sn = field;
""" + guard + r"""
    return (int)len;
err:
    return ret;
}
int main(void)
{
    /* Encoded length excludes the 3-dword XDomain header: 15 fixed
       payload dwords, followed by 1/2/45 data dwords. */
    assert(data_dwords(0) == -EINVAL);
    assert(data_dwords(14) == -EINVAL);
    assert(data_dwords(15) == -EINVAL); /* no progress: must reject */
    assert(data_dwords(16) == 1);       /* 76-byte final response */
    assert(data_dwords(17) == 2);       /* 80-byte final response */
    assert(data_dwords(60) == 45);      /* 252-byte full response */
    return 0;
}
"""
        with tempfile.TemporaryDirectory(prefix="xdomain-length-") as tmp:
            code = Path(tmp) / "check.c"
            output = Path(tmp) / "check"
            code.write_text(c)
            subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra",
                            "-Werror", "-fsanitize=undefined", "-fno-sanitize-recover=all",
                            str(code), "-o", str(output)], check=True, timeout=30)
            subprocess.run([str(output)], check=True, timeout=15)


if __name__ == "__main__":
    unittest.main()
