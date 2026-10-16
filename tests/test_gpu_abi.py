# SPDX-License-Identifier: GPL-2.0-only
"""Compare every legacy ioctl number/layout against the immutable capture."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[1]

class GpuAbiTests(unittest.TestCase):
    def test_legacy_layout_unchanged(self):
        with tempfile.TemporaryDirectory(prefix="sgu-abi-") as tmp:
            tmp = Path(tmp)
            source = Path(os.environ.get("USB4_SOURCE_TREE", tmp / "source"))
            if "USB4_SOURCE_TREE" not in os.environ:
                subprocess.run(["python3", str(REPO / "tools/prepare_overlay.py"), str(source)], check=True)
            program = tmp / "abi.c"
            types = ("fence", "query", "buffer", "import", "prepare", "action", "frame_result", "event")
            commands = ("QUERY", "IMPORT", "ARM", "PREPARE", "SUBMIT", "EVENT", "CANCEL", "RX_REPOST", "RETIRE", "STOP", "RELEASE")
            program.write_text('#include <stdio.h>\n#include <stddef.h>\n#include <linux/strix_gpu_stream.h>\nint main(void) {\n' +
                ''.join(f'printf("%zu\\n", sizeof(struct sgu_{t}));\n' for t in types) +
                ''.join(f'printf("%lu\\n", (unsigned long)SGU_IOC_{c});\n' for c in commands) +
                'printf("%zu %zu %zu %u %u %llu\\n", offsetof(struct sgu_prepare, payload_bytes), offsetof(struct sgu_event, frames), offsetof(struct sgu_fence, connection_epoch), SGU_ABI_MAJOR, SGU_ABI_MINOR, (unsigned long long)SGU_ABI_COOKIE); return 0; }\n')
            outputs = []
            for index, root in enumerate((REPO / "overlay", source)):
                binary = tmp / f"abi{index}"
                subprocess.run([os.environ.get("CC", "cc"), "-Wall", "-Wextra", "-Werror",
                                "-I", str(root / "include/uapi"), str(program), "-o", str(binary)], check=True)
                outputs.append(subprocess.check_output([str(binary)]))
            self.assertEqual(outputs[0], outputs[1])

if __name__ == "__main__":
    unittest.main()
