"""Exercise the driver's actual write-window validator with ordinary GPT image fragments."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from disk import fragments


class USBDataTests(unittest.TestCase):
    def test_validated_data_window_protects_other_partitions(self):
        with tempfile.TemporaryDirectory() as scratch:
            folder = Path(scratch)
            roots = []
            for name, size in (('os', 2 << 20), ('systemos', 2 << 20), ('data', 8 << 30)):
                path = folder / name
                with path.open('wb') as stream:
                    stream.truncate(size)
                roots.append(path)
            parts = fragments(b'CHROMEOS' + bytes(65536 - 8), disk_mib=8400, kernel_mib=96, roots=roots)
            last = 8400 * 2048 - 1
            fixture = bytearray(67 * 512)
            for offset, data in parts:
                if isinstance(data, Path):
                    continue
                if offset < 34 * 512:
                    fixture[offset:offset + len(data)] = data
                elif offset >= (last - 32) * 512:
                    start = offset - (last - 32) * 512 + 34 * 512
                    fixture[start:start + len(data)] = data
            image = folder / 'gpt.bin'
            image.write_bytes(fixture)
            binary = folder / 'window-test'
            subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                            '-I' + str(ROOT / 'platform/coachz-prelink/drivers/RamBlock'),
                            str(ROOT / 'tests/usb_data_window.cpp'), '-o', str(binary)], check=True)
            subprocess.run([str(binary), str(image)], check=True)
