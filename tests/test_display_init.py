"""Keep the owned display constructor deferred without changing executable bytes."""
import importlib.util
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('coachz_late_init', ROOT / 'platform/coachz-prelink/scripts/coachz_late_init.py')
helper = importlib.util.module_from_spec(spec)
spec.loader.exec_module(helper)


class DisplayInitTests(unittest.TestCase):
    def test_actual_arm64_constructor_is_deferred_and_exported(self):
        with tempfile.TemporaryDirectory() as scratch:
            folder = Path(scratch)
            source = folder / 'late.cpp'
            source.write_text('extern void run(); struct Late { Late() { run(); } }; Late instance;\n')
            before, after = folder / 'before.o', folder / 'after.o'
            subprocess.run(['xcrun', 'clang++', '-target', 'arm64-apple-ios17.0', '-c', str(source), '-o', str(before)], check=True)
            raw = before.read_bytes()
            symbol = '__GLOBAL__sub_I_late.cpp'
            updated = helper.stage_initializer(raw, symbol)
            after.write_bytes(updated)
            self.assertEqual(len(raw), len(updated))
            self.assertNotIn(b'__mod_init_func', updated)
            self.assertIn(b'__coachz_init', updated)
            self.assertIn(symbol, subprocess.check_output(['xcrun', 'nm', '-g', str(after)], text=True))
            self.assertNotIn(symbol, subprocess.check_output(['xcrun', 'nm', '-g', str(before)], text=True))
            # Segment payload includes instructions, pointers and relocation entries: all stay byte-identical.
            cursor = 32
            for _ in range(struct.unpack_from('<I', raw, 16)[0]):
                command, length = struct.unpack_from('<II', raw, cursor)
                if command == 0x19:
                    start, size = struct.unpack_from('<QQ', raw, cursor + 40)
                    self.assertEqual(raw[start:start + size], updated[start:start + size])
                cursor += length
            for invalid, name in ((raw[:31], symbol), (raw, 'missing'), (updated, symbol)):
                with self.assertRaises(ValueError):
                    helper.stage_initializer(invalid, name)
