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


def layout(disk_mib, kernel_mib):
    if (type(disk_mib) is not int or type(kernel_mib) is not int or
            not 4 <= disk_mib <= 1024 or not 1 <= kernel_mib <= disk_mib - 2):
        raise ValueError("invalid disk/kernel MiB sizes")
    return disk_mib * 2048, kernel_mib * 2048


def fragments(payload, label="Coachz probe", disk_mib=64, kernel_mib=16, tries=1):
    sectors, length = layout(disk_mib, kernel_mib)
    if type(tries) is not int or not 1 <= tries <= 15:
        raise ValueError("GPT tries must be 1..15")
    if not 65536 <= len(payload) <= length * SECTOR or payload[:8] != b"CHROMEOS":
        raise ValueError("expected a signed vboot kernel container within the kernel partition")
    name = label.encode("utf-16-le")
    if len(name) > 72 or "\0" in label:
        raise ValueError("GPT label too long or contains NUL")
    disk_guid = uuid.uuid5(uuid.NAMESPACE_URL, "urn:trogdor-ipados:" + hashlib.sha256(payload).hexdigest())
    part_guid = uuid.uuid5(disk_guid, "kernel")
    entries = bytearray(ENTRY_COUNT * ENTRY_SIZE)
    # Priority 1, successful=0. No other partitions are bootable. The firmware uses up one
    # try per boot and then marks the partition bad, so the medium needs rewriting.
    attrs = (1 << 48) | (tries << 52)
    struct.pack_into("<16s16sQQQ72s", entries, 0, KERNEL_TYPE.bytes_le,
                     part_guid.bytes_le, FIRST, FIRST + length - 1, attrs, name)
    array_crc = zlib.crc32(entries)
    def header(current, alternate, array_lba):
        h = bytearray(SECTOR)
        struct.pack_into("<8sIIIIQQQQ16sQIII", h, 0, b"EFI PART", 0x10000, 92,
                         0, 0, current, alternate, 34, sectors - 34,
                         disk_guid.bytes_le, array_lba, ENTRY_COUNT, ENTRY_SIZE, array_crc)
        struct.pack_into("<I", h, 16, zlib.crc32(h[:92]))
        return bytes(h)
    mbr = bytearray(SECTOR)
    struct.pack_into("<B3sB3sII", mbr, 446, 0, b"\0\2\0", 0xee,
                     b"\xff\xff\xff", 1, sectors - 1)
    mbr[510:512] = b"\x55\xaa"
    return [(0, mbr), (SECTOR, header(1, sectors - 1, 2)), (2 * SECTOR, entries),
            (FIRST * SECTOR, payload), ((sectors - 33) * SECTOR, entries),
            ((sectors - 1) * SECTOR, header(sectors - 1, 1, sectors - 33))]


def create_image(source, output, label="Coachz probe", disk_mib=64, kernel_mib=16, tries=1):
    sectors, length = layout(disk_mib, kernel_mib)
    source, output = Path(source).resolve(), Path(output).resolve()
    if not source.is_file():
        raise ValueError("input must be a regular file")
    if source.stat().st_size > length * SECTOR:
        raise ValueError("kernel container exceeds partition")
    if output == source or output.suffix != ".img" or output.is_relative_to(Path("/dev")):
        raise ValueError("output must be a separate .img file outside /dev")
    parts = fragments(source.read_bytes(), label, disk_mib, kernel_mib, tries)
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("xb") as dest:
        dest.truncate(sectors * SECTOR)
        for offset, data in parts:
            dest.seek(offset)
            dest.write(data)
    return output


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("kernel", type=Path)
    p.add_argument("output", type=Path)
    p.add_argument("--label", default="Coachz probe")
    p.add_argument("--disk-mib", type=int, default=64)
    p.add_argument("--kernel-mib", type=int, default=16)
    a = p.parse_args()
    print("Created ordinary GPT image:", create_image(a.kernel, a.output, a.label, a.disk_mib, a.kernel_mib))


if __name__ == "__main__":
    main()
