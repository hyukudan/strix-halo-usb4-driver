# SPDX-License-Identifier: GPL-2.0-only
"""Execute the real GPU extension in a userspace API simulator with sanitizers.

Not a kernel build or hardware emulator. The complete driver implementation is
compiled, but kernel locking, reservation objects, DMA, workqueues and FD APIs
are test doubles. Pinned-kernel object builds and hardware testing remain gates.
"""
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[1]
CASES = ("geometry", "queue", "failure_isolation", "events_legacy", "timeout_recovery",
         "wait_concurrency", "wait_retry_order", "export_lifetime", "admission_rollback", "watchdog_schedule", "max_queue_reuse")

class GpuQueueTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="sgu-queue-")
        cls.addClassCleanup(cls.temp.cleanup)
        tmp = Path(cls.temp.name)
        if "USB4_SOURCE_TREE" in os.environ:
            source = Path(os.environ["USB4_SOURCE_TREE"])
        else:
            source = tmp / "source"
            subprocess.run(["python3", str(REPO / "tools/prepare_overlay.py"), str(source)], check=True)
        implementation = (source / "drivers/thunderbolt/gpu_stream.c").read_text()
        # Replace ONLY the includes; all function bodies are the actual source.
        implementation = re.sub(r"^#include[^\n]*\n", "", implementation, flags=re.MULTILINE)
        unit = tmp / "test.c"
        unit.write_text('#include "sgu_test_kernel.h"\n#include "gpu_stream_logic.h"\n' +
                        implementation + '\n#include "sgu_queue_cases.h"\n')
        cls.binary = tmp / "test"
        compiler = os.environ.get("CC", "cc")
        if not shutil.which(compiler):
            raise RuntimeError(f"Required C compiler unavailable: {compiler}")
        command = [compiler, "-std=gnu11", "-g", "-O1", "-Wall", "-Wextra", "-Werror",
                   "-Wno-unused-function", "-Wno-unused-parameter", "-Wno-unused-const-variable",
                   "-fno-omit-frame-pointer", "-fsanitize=address,undefined", "-pthread",
                   "-I", str(REPO / "tests"), "-I", str(source / "drivers/thunderbolt"),
                   "-I", str(source / "include/uapi"), str(unit), "-o", str(cls.binary)]
        result = subprocess.run(command, capture_output=True, text=True)
        if result.returncode:
            raise RuntimeError(result.stdout + result.stderr)

    def run_case(self, case):
        result = subprocess.run([str(self.binary), case], capture_output=True, text=True,
                                timeout=20, env={**os.environ, "ASAN_OPTIONS": "detect_leaks=1:abort_on_error=1"})
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn(f"PASS {case}", result.stdout)

for _case in CASES:
    setattr(GpuQueueTests, f"test_{_case}", lambda self, case=_case: self.run_case(case))

if __name__ == "__main__":
    unittest.main()
