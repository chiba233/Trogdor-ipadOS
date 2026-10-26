#!/usr/bin/env python3
"""Create a small ChromeOS GPT image as a new regular file; never write devices."""
import argparse
import hashlib
from pathlib import Path
import struct
import uuid
import zlib

SECTOR = 512
SECTORS = 131072  # 64 MiB
ENTRY_COUNT = 128
ENTRY_SIZE = 128
FIRST = 2048
LENGTH = 32768  # 16 MiB kernel partition
KERNEL_TYPE = uuid.UUID("fe3a2a5d-4f32-41a7-b725-accc3285a309")


def fragments(payload, label="Coachz probe"):
    if not 65536 <= len(payload) <= LENGTH * SECTOR or payload[:8] != b"CHROMEOS":
        raise ValueError("expected a signed vboot kernel container within 16 MiB")
    name = label.encode("utf-16-le")
    if len(name) > 72 or "\0" in label:
        raise ValueError("GPT label too long or contains NUL")
    disk_guid = uuid.uuid5(uuid.NAMESPACE_URL, "urn:trogdor-ipados:" + hashlib.sha256(payload).hexdigest())
    part_guid = uuid.uuid5(disk_guid, "kernel")
    entries = bytearray(ENTRY_COUNT * ENTRY_SIZE)
    # Priority 1, one attempt, successful=0. No other partitions are bootable.
    attrs = (1 << 48) | (1 << 52)
    struct.pack_into("<16s16sQQQ72s", entries, 0, KERNEL_TYPE.bytes_le,
                     part_guid.bytes_le, FIRST, FIRST + LENGTH - 1, attrs, name)
    array_crc = zlib.crc32(entries)
    def header(current, alternate, array_lba):
        h = bytearray(SECTOR)
        struct.pack_into("<8sIIIIQQQQ16sQIII", h, 0, b"EFI PART", 0x10000, 92,
                         0, 0, current, alternate, 34, SECTORS - 34,
                         disk_guid.bytes_le, array_lba, ENTRY_COUNT, ENTRY_SIZE, array_crc)
        struct.pack_into("<I", h, 16, zlib.crc32(h[:92]))
        return bytes(h)
    mbr = bytearray(SECTOR)
    struct.pack_into("<B3sB3sII", mbr, 446, 0, b"\0\2\0", 0xee,
                     b"\xff\xff\xff", 1, SECTORS - 1)
    mbr[510:512] = b"\x55\xaa"
    return [(0, mbr), (SECTOR, header(1, SECTORS - 1, 2)), (2 * SECTOR, entries),
            (FIRST * SECTOR, payload), ((SECTORS - 33) * SECTOR, entries),
            ((SECTORS - 1) * SECTOR, header(SECTORS - 1, 1, SECTORS - 33))]


def create_image(source, output, label="Coachz probe"):
    source, output = Path(source).resolve(), Path(output).resolve()
    if not source.is_file():
        raise ValueError("input must be a regular file")
    if source.stat().st_size > LENGTH * SECTOR:
        raise ValueError("kernel container exceeds partition")
    if output == source or output.suffix != ".img" or output.is_relative_to(Path("/dev")):
        raise ValueError("output must be a separate .img file outside /dev")
    parts = fragments(source.read_bytes(), label)
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("xb") as dest:
        dest.truncate(SECTORS * SECTOR)
        for offset, data in parts:
            dest.seek(offset)
            dest.write(data)
    return output


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("kernel", type=Path)
    p.add_argument("output", type=Path)
    p.add_argument("--label", default="Coachz probe")
    a = p.parse_args()
    print("Created ordinary GPT image:", create_image(a.kernel, a.output, a.label))


if __name__ == "__main__":
    main()
