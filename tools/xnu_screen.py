#!/usr/bin/env python3
"""Pack a source-XNU screen-output payload: no checkpoint patch, no return to Linux."""
import argparse
import hashlib
from pathlib import Path

from xnu_checkpoint import HEADER, MAGIC, adt_fields
from xnu_macho import macho

VERSION = 12
COMMAND_LINE = '-v debug=0xc serial=3 cpus=1 keepsyms=1 -enable_kprintf_spam'


def pack(data, adt, window, memory, command_line=COMMAND_LINE):
    if window & 0x1fffff or memory & 0x1fffff:
        raise ValueError('window base and size must be 2 MiB aligned')
    kernel = macho(data)
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
    span = kernel.end - kernel.base
    header = HEADER.pack(MAGIC, VERSION, len(kernel.segments), kernel.base, kernel.entry, window, 0, raw_at, len(data),
                         adt_at, len(adt), span, memory, base_field, size_field, affinity, frequency, raw)
    payload = header + table + bytes(raw_at - HEADER.size - len(table)) + data
    return payload + bytes(adt_at - len(payload)) + adt


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
