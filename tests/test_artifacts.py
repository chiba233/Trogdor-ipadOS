import ctypes
import hashlib
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from artifacts import diagnostic_dtb, extract_image, fit, inspect_image
from fdt import Node, encode, properties, string, u32

class FdtTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        library = Path(cls.temp.name) / ("parser.dylib" if sys.platform == "darwin" else "parser.so")
        compiler = shutil.which("cc")
        if not compiler:
            raise RuntimeError("host C compiler required")
        subprocess.run([compiler, "-shared", "-fPIC", "-Wall", "-Wextra", "-Werror",
                        str(ROOT / "boot/fdt.c"), "-o", str(library)], check=True)
        cls.lib = ctypes.CDLL(str(library))
        cls.lib.fdt_valid.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
        cls.lib.fdt_valid.restype = ctypes.c_int
        cls.lib.fdt_property.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_char_p,
                                       ctypes.c_char_p, ctypes.POINTER(ctypes.c_uint32)]
        cls.lib.fdt_property.restype = ctypes.c_void_p

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def valid(self, blob, cap=None):
        buffer = ctypes.create_string_buffer(blob)
        return self.lib.fdt_valid(buffer, len(blob) if cap is None else cap)

    def test_firmware_injected_properties_and_empty_values(self):
        blob = encode(Node("", {"compatible": string("google,coachz")}, [
            Node("chosen", {"linux,initrd-start": u32(0xd0100000), "empty": b""}),
            Node("firmware", children=[Node("coreboot", {"board-id": u32(3)})])]))
        self.assertTrue(self.valid(blob))
        buf = ctypes.create_string_buffer(blob)
        size = ctypes.c_uint32()
        ptr = self.lib.fdt_property(buf, len(blob), b"/firmware/coreboot", b"board-id", ctypes.byref(size))
        self.assertEqual(ctypes.string_at(ptr, size.value), u32(3))
        ptr = self.lib.fdt_property(buf, len(blob), b"/chosen", b"empty", ctypes.byref(size))
        self.assertTrue(ptr)
        self.assertEqual(size.value, 0)

    def test_pinned_depthcharge_structure_size_excludes_end_token(self):
        blob = bytearray(diagnostic_dtb())
        size = struct.unpack_from('>I', blob, 36)[0]
        struct.pack_into('>I', blob, 36, size - 4)
        self.assertTrue(self.valid(bytes(blob)))

    def test_every_truncation_is_rejected(self):
        blob = diagnostic_dtb()
        for n in range(len(blob)):
            with self.subTest(length=n):
                self.assertFalse(self.valid(blob[:n]))

    def test_out_of_bounds_and_unterminated_headers(self):
        blob = diagnostic_dtb()
        for offset, value in ((4, 0xffffffff), (8, len(blob)), (12, len(blob)),
                              (16, len(blob) - 8), (20, 18), (32, 0xffffffff), (36, 0xffffffff)):
            bad = bytearray(blob)
            struct.pack_into(">I", bad, offset, value)
            self.assertFalse(self.valid(bytes(bad)))

    def test_oversized_depth_and_missing_end(self):
        root = Node("")
        child = root
        for _ in range(34):
            next_node = Node("x")
            child.children.append(next_node)
            child = next_node
        self.assertFalse(self.valid(encode(root)))
        bad = bytearray(diagnostic_dtb())
        struct_start = struct.unpack_from(">I", bad, 8)[0]
        struct_size = struct.unpack_from(">I", bad, 36)[0]
        struct.pack_into(">I", bad, struct_start + struct_size - 4, 4)
        self.assertFalse(self.valid(bytes(bad)))

class ArtifactTests(unittest.TestCase):
    def image(self):
        return struct.pack("<II6QII", 0x14000010, 0, 0, 4096, 0xa, 0, 0, 0, 0x644d5241, 0) + bytes(128)

    def test_fit_contains_actual_image_dtb_and_hashes(self):
        image, dtb = self.image(), diagnostic_dtb()
        p = properties(fit(image, dtb))
        for name, content in (("kernel", image), ("fdt", dtb)):
            self.assertEqual(p[("/images/" + name, "data")], content)
            self.assertEqual(p[("/images/" + name + "/hash-1", "value")], hashlib.sha256(content).digest())
        self.assertEqual(p[("/configurations/conf-coachz", "compatible")], string("google,coachz"))

    def test_invalid_image_cannot_be_packaged(self):
        for offset, value in ((0, 0), (24, 1), (56, 0)):
            bad = bytearray(self.image())
            struct.pack_into("<I", bad, offset, value)
            with self.assertRaises(ValueError):
                fit(bytes(bad), diagnostic_dtb())

    def test_elf_load_segments_are_bounded(self):
        path = ROOT / "build/memory/probe.elf"
        if not path.exists():
            self.skipTest("run tools/build.py")
        elf = bytearray(path.read_bytes())
        self.assertEqual(extract_image(elf), (ROOT / "build/memory/Image").read_bytes())
        phoff = struct.unpack_from("<Q", elf, 32)[0]
        struct.pack_into("<Q", elf, phoff + 8, len(elf))
        with self.assertRaises(ValueError):
            extract_image(elf)

    def test_built_probe_layout(self):
        manifest = ROOT / "build/manifest.json"
        if not manifest.is_file():
            self.skipTest("run tools/build.py for compiled artifact checks")
        for mode in ("memory", "uart"):
            image = (ROOT / "build" / mode / "Image").read_bytes()
            header = inspect_image(image)
            self.assertGreater(header["memory_size"], len(image))
            self.assertEqual(properties((ROOT / "build" / mode / "probe.itb").read_bytes())
                             [("/images/kernel", "data")], image)

if __name__ == "__main__":
    unittest.main()
