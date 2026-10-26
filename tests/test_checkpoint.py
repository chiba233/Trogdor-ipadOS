import ctypes
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class CheckpointTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        library = Path(cls.temp.name) / ("checkpoint.dylib" if sys.platform == "darwin" else "checkpoint.so")
        subprocess.run([shutil.which("cc"), "-shared", "-fPIC", "-DCHECKPOINT_HOST_TEST",
                        "-Wall", "-Wextra", "-Werror", str(ROOT / "boot/checkpoint.c"),
                        "-o", str(library)], check=True)
        cls.lib = ctypes.CDLL(str(library))
        cls.lib.checkpoint_delay.argtypes = [ctypes.c_uint, ctypes.c_uint,
                                             ctypes.c_uint, ctypes.c_int32, ctypes.c_uint64]
        cls.lib.checkpoint_delay.restype = ctypes.c_uint

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def test_distinct_success_checkpoints(self):
        delay = self.lib.checkpoint_delay
        self.assertEqual(delay(2, 2, 3, 2, 19200000), 3)
        self.assertEqual(delay(1, 1, 3, 0x10001, 19200000), 10)

    def test_wrong_el_failed_diagnostics_and_unsupported_psci_never_reset(self):
        delay = self.lib.checkpoint_delay
        for args in ((2, 1, 3, 2, 19200000), (1, 2, 3, 2, 19200000),
                     (0, 0, 3, 2, 19200000), (3, 3, 3, 2, 19200000),
                     (1, 1, 2, 2, 19200000), (1, 1, 255, 2, 19200000),
                     (1, 1, 3, -1, 19200000), (1, 1, 3, 1, 19200000),
                     (1, 1, 3, 2, 0), (1, 1, 3, 2, 1000000001)):
            with self.subTest(args=args):
                self.assertEqual(delay(*args), 0)


if __name__ == "__main__":
    unittest.main()
