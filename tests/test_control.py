from pathlib import Path
import struct
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from control import inhibit_reset, NOP, RESET, SMC, VERSION


class MatchedControlTests(unittest.TestCase):
    def fixture(self):
        header = struct.pack("<II6QII", 0x14000010, 0, 0, 4096, 0xa,
                             0, 0, 0, 0x644d5241, 0)
        code = VERSION + NOP * 4 + RESET + bytes.fromhex("c0035fd6")
        return header + code + NOP * 4, len(code)

    def test_only_final_smc_changes_and_version_call_is_preserved(self):
        image, size = self.fixture()
        result, at = inhibit_reset(image, 64, size)
        self.assertEqual(result[:at], image[:at])
        self.assertEqual(result[at + 4:], image[at + 4:])
        self.assertEqual(result[at:at + 4], NOP)
        self.assertEqual(image[at:at + 4], SMC)
        self.assertEqual(result[64:64 + len(VERSION)], VERSION)
        self.assertEqual(len(result), len(image))

    def test_wrong_psci_function_or_extra_smc_is_rejected(self):
        image, size = self.fixture()
        for at, word in ((64, NOP), (64 + len(VERSION), SMC),
                         (64 + len(VERSION) + 16, NOP)):
            bad = image[:at] + word + image[at + 4:]
            with self.subTest(offset=at), self.assertRaises(ValueError):
                inhibit_reset(bad, 64, size)

    def test_invalid_symbol_range_is_rejected(self):
        image, size = self.fixture()
        for address, length in ((0, size), (65, size), (64, 0),
                                (64, size - 1), (64, len(image))):
            with self.subTest(address=address, size=length), self.assertRaises(ValueError):
                inhibit_reset(image, address, length)

    def test_control_cannot_be_patched_twice(self):
        image, size = self.fixture()
        control, _ = inhibit_reset(image, 64, size)
        with self.assertRaises(ValueError):
            inhibit_reset(control, 64, size)
