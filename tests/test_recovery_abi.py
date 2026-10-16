# SPDX-License-Identifier: GPL-2.0-only
"""All existing command values and structures must retain their ABI."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[1]
SOURCE = Path(os.environ.get('USB4_SOURCE_TREE', ROOT / 'audited-overlay'))
class RecoveryAbiTests(unittest.TestCase):
    def test_legacy_commands_and_layout(self):
        types = ('fence','query','buffer','import','prepare','action','frame_result','event')
        commands = ('QUERY','IMPORT','ARM','PREPARE','SUBMIT','EVENT','CANCEL','RX_REPOST','RETIRE','STOP','RELEASE')
        code = '#include <stdio.h>\n#include <stddef.h>\n#include <linux/strix_gpu_stream.h>\nint main(void) {\n'
        code += ''.join(f'printf("%zu\\n", sizeof(struct sgu_{t}));\n' for t in types)
        code += ''.join(f'printf("%lu\\n", (unsigned long)SGU_IOC_{c});\n' for c in commands)
        code += 'printf("%zu %zu %u %u %llu\\n", offsetof(struct sgu_prepare, payload_bytes), offsetof(struct sgu_event, frames), SGU_ABI_MAJOR, SGU_ABI_MINOR, (unsigned long long)SGU_ABI_COOKIE); return 0; }\n'
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            (tmp / 'abi.c').write_text(code)
            results=[]
            for index, base in enumerate((ROOT/'overlay', SOURCE)):
                binary=tmp/f'abi{index}'
                subprocess.run([os.environ.get('CC','cc'),'-Wall','-Wextra','-Werror','-I',str(base/'include/uapi'),str(tmp/'abi.c'),'-o',str(binary)],check=True)
                results.append(subprocess.check_output([str(binary)]))
            self.assertEqual(*results)
