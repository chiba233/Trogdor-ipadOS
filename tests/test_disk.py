from pathlib import Path
import struct
import sys
import tempfile
import unittest
import uuid
import zlib

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from disk import create_image, fragments, APFS_TYPE, FIRST, LENGTH, SECTOR, SECTORS, KERNEL_TYPE


class DiskTests(unittest.TestCase):
    def payload(self):
        # Synthetic container checks layout only, not a cryptographic signature.
        return b"CHROMEOS" + bytes(65536 - 8)

    def test_primary_backup_crc_payload_and_single_kernel_partition(self):
        with tempfile.TemporaryDirectory() as d:
            source, output = Path(d) / "probe.kpart", Path(d) / "usb.img"
            source.write_bytes(self.payload())
            create_image(source, output, "EL1 10s")
            b = output.read_bytes()
        self.assertEqual(len(b), SECTORS * SECTOR)
        self.assertEqual(b[510:512], b"\x55\xaa")
        self.assertEqual(b[450], 0xee)
        arrays = []
        for current, alternate in ((1, SECTORS - 1), (SECTORS - 1, 1)):
            h = bytearray(b[current * SECTOR:current * SECTOR + 92])
            self.assertEqual(h[:8], b"EFI PART")
            stored = struct.unpack_from("<I", h, 16)[0]
            struct.pack_into("<I", h, 16, 0)
            self.assertEqual(stored, zlib.crc32(h))
            self.assertEqual(struct.unpack_from("<QQ", h, 24), (current, alternate))
            array_lba, count, stride, crc = struct.unpack_from("<QIII", h, 72)
            entries = b[array_lba * SECTOR:array_lba * SECTOR + count * stride]
            self.assertEqual(zlib.crc32(entries), crc)
            arrays.append(entries)
        self.assertEqual(arrays[0], arrays[1])
        entry = arrays[0][:128]
        self.assertEqual(uuid.UUID(bytes_le=entry[:16]), KERNEL_TYPE)
        first, last, attrs = struct.unpack_from("<QQQ", entry, 32)
        self.assertEqual((first, last), (FIRST, FIRST + LENGTH - 1))
        self.assertEqual((attrs >> 48) & 15, 1)
        self.assertEqual((attrs >> 52) & 15, 1)
        self.assertEqual((attrs >> 56) & 1, 0)
        self.assertFalse(any(arrays[0][128:]))
        self.assertEqual(b[first * SECTOR:first * SECTOR + len(self.payload())], self.payload())

    def test_tries_default_to_one_and_stay_in_the_gpt_field(self):
        data = b"CHROMEOS" + bytes(65536)
        attrs = lambda **kw: struct.unpack_from("<Q", fragments(data, **kw)[2][1], 48)[0]
        self.assertEqual(attrs(), (1 << 48) | (1 << 52))
        self.assertEqual(attrs(tries=15), (1 << 48) | (15 << 52))
        for tries in (0, 16, True):
            with self.assertRaises(ValueError):
                fragments(data, tries=tries)

    def test_invalid_unsigned_oversized_containers_and_labels_are_rejected(self):
        for data in (b"CHROMEOS", bytes(65536), b"CHROMEOS" + bytes(LENGTH * SECTOR)):
            with self.assertRaises(ValueError):
                fragments(data)
        for label in ("x" * 37, "bad\0name"):
            with self.assertRaises(ValueError):
                fragments(self.payload(), label)

    def test_optional_root_container_follows_the_kernel_partition(self):
        root = b"NXSB" + bytes(3 << 20)
        parts = fragments(self.payload(), disk_mib=64, kernel_mib=16, roots=[root])
        entries = parts[2][1]
        self.assertEqual(uuid.UUID(bytes_le=bytes(entries[128:144])), APFS_TYPE)
        first, last, attrs = struct.unpack_from("<QQQ", entries, 128 + 32)
        self.assertEqual((first, last - first + 1, attrs), (FIRST + LENGTH, 4 * 2048, 0))
        self.assertIn((first * SECTOR, root), parts)
        self.assertFalse(any(entries[256:]))
        self.assertEqual(fragments(self.payload())[2][1][128:], bytes(127 * 128))
        for oversized in (b"", bytes((64 - 16) << 20)):
            with self.assertRaises(ValueError):
                fragments(self.payload(), roots=[oversized])
        second = b"NXSB" + bytes(1 << 20)
        entries = fragments(self.payload(), roots=[root, second])[2][1]
        self.assertEqual(struct.unpack_from("<Q", entries, 256 + 32)[0], FIRST + LENGTH + 4 * 2048)
        # The GPT follows the layout, not the kernel: a kernel-only image matches the medium.
        other = b"CHROMEOS" + bytes(65536)
        self.assertEqual(fragments(other, roots=[root])[1], fragments(self.payload(), roots=[root])[1])
        self.assertNotEqual(fragments(other)[1], fragments(self.payload())[1])
        with tempfile.TemporaryDirectory() as d:
            source, container = Path(d) / "probe.kpart", Path(d) / "root.apfs"
            source.write_bytes(self.payload())
            container.write_bytes(root)
            full = create_image(source, Path(d) / "full.img", roots=[container]).read_bytes()
            head = create_image(source, Path(d) / "head.img", roots=[container], kernel_only=True).read_bytes()
        self.assertEqual(len(head), (FIRST + LENGTH) * SECTOR)
        self.assertEqual(full[:len(head)], head)
        self.assertEqual(full[(FIRST + LENGTH) * SECTOR:][:len(root)], root)

    def test_existing_file_and_device_output_are_refused(self):
        with tempfile.TemporaryDirectory() as d:
            source, output = Path(d) / "probe.kpart", Path(d) / "usb.img"
            source.write_bytes(self.payload())
            output.write_bytes(b"keep")
            with self.assertRaises(FileExistsError):
                create_image(source, output)
            self.assertEqual(output.read_bytes(), b"keep")
            with self.assertRaises(ValueError):
                create_image(source, Path("/dev/coachz-probe.img"))


if __name__ == "__main__":
    unittest.main()
