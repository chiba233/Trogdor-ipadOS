#!/usr/bin/env python3
"""Pack a source-XNU screen-output payload: no checkpoint patch, no return to Linux."""
import argparse
import hashlib
from pathlib import Path
import struct

from xnu_checkpoint import HEADER, MAGIC, adt_fields
from xnu_init import ramdisk as init_ramdisk
from xnu_macho import macho

VERSION = 12
# Without prelinked components the type distribution is skewed: the default budget of
# 260 gives one size class more than the 32 zones kalloc allows.
COMMAND_LINE = '-v debug=0xc serial=3 cpus=1 keepsyms=1 -enable_kprintf_spam kt_fixed_zones=120 rd=md0 cs_enforcement_disable=1'


def up(value):
    return (value + 0x3fff) & ~0x3fff


def add_ramdisk_property(adt, base, size):
    """Add /chosen/memory-map RAMDisk, read by IOFindBSDRoot as (physical base, size)."""
    at = 0
    def visit():
        nonlocal at
        node = at
        props, children = struct.unpack_from('<II', adt, at)
        at += 8
        name = b''
        for _ in range(props):
            key = adt[at:at + 32].split(b'\0')[0]
            length = struct.unpack_from('<I', adt, at + 32)[0] & 0x7fffffff
            if key == b'name':
                name = adt[at + 36:at + 36 + length].split(b'\0')[0]
            at = (at + 36 + length + 3) & ~3
        if name == b'memory-map':
            return node, at
        for _ in range(children):
            found = visit()
            if found:
                return found
        return None
    found = visit()
    if not found:
        raise ValueError('ADT has no memory-map node')
    node, end = found
    prop = b'RAMDisk'.ljust(32, b'\0') + struct.pack('<IQQ', 16, base, size)
    result = bytearray(adt[:end]) + prop + adt[end:]
    struct.pack_into('<I', result, node, struct.unpack_from('<I', adt, node)[0] + 1)
    return result


def pack(data, adt, window, memory, command_line=COMMAND_LINE, ramdisk=None):
    if window & 0x1fffff or memory & 0x1fffff:
        raise ValueError('window base and size must be 2 MiB aligned')
    kernel = macho(data)
    ramdisk = init_ramdisk() if ramdisk is None else ramdisk
    span = kernel.end - kernel.base
    bias = kernel.base & 0x1fffff
    # Same placement as boot/xnu_screen.c: kernel, boot_args page, ADT, RAM disk.
    tree = up(span + bias) + 4096
    adt = add_ramdisk_property(adt, window + up(tree + len(adt) + 52), len(ramdisk))
    base_field, size_field, affinity, frequency = adt_fields(adt)
    adt = bytearray(adt)
    for at in (base_field, size_field):
        adt[at:at + 8] = bytes(8)
    raw = command_line.encode('ascii')
    if len(raw) >= 1024:
        raise ValueError('invalid command line')
    table = b''.join((va - kernel.base).to_bytes(8, 'little') + size.to_bytes(8, 'little') +
                     off.to_bytes(8, 'little') + length.to_bytes(8, 'little') + prot.to_bytes(8, 'little')
                     for va, size, off, length, prot in kernel.segments)
    raw_at = (HEADER.size + len(table) + 15) & ~15
    adt_at = (raw_at + len(data) + 15) & ~15
    ramdisk_at = adt_at + len(adt)
    header = HEADER.pack(MAGIC, VERSION, len(kernel.segments), kernel.base, kernel.entry, window, ramdisk_at, raw_at, len(data),
                         adt_at, len(adt), span, memory, base_field, size_field, affinity, frequency, raw)
    payload = header + table + bytes(raw_at - HEADER.size - len(table)) + data
    return payload + bytes(adt_at - len(payload)) + adt + ramdisk


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--kernel', type=Path, required=True)
    p.add_argument('--adt', type=Path, required=True)
    p.add_argument('--window', type=lambda v: int(v, 0), required=True, help='physical base; choose it from the private device tree')
    p.add_argument('--memory', type=lambda v: int(v, 0), default=1 << 30)
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    if a.output.exists() or a.output.resolve().is_relative_to(Path('/dev')):
        p.error('output must be a new ordinary file')
    payload = pack(a.kernel.read_bytes(), a.adt.read_bytes(), a.window, a.memory)
    a.output.parent.mkdir(parents=True, exist_ok=True)
    a.output.write_bytes(payload)
    print('Built screen-output XNU payload', hashlib.sha256(payload).hexdigest())


if __name__ == '__main__':
    main()
