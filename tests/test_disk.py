from pathlib import Path
import struct
import sys
import tempfile
import unittest
import uuid
import zlib

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from disk import create_image, fragments, FIRST, LENGTH, SECTOR, SECTORS, KERNEL_TYPE


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

    def test_invalid_unsigned_oversized_containers_and_labels_are_rejected(self):
        for data in (b"CHROMEOS", bytes(65536), b"CHROMEOS" + bytes(LENGTH * SECTOR)):
            with self.assertRaises(ValueError):
                fragments(data)
        for label in ("x" * 37, "bad\0name"):
            with self.assertRaises(ValueError):
                fragments(self.payload(), label)

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
