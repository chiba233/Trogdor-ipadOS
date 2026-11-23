#!/usr/bin/env python3
"""Merge version 2 trust cache modules into one (for tools/xnu_screen.py --root-trust-cache).

k224b and k225 loaded nine trust cache modules and AMFI rejected the camera viewer as ad hoc although its cdhash was in one of
them; k222 loaded eight and the viewer ran. Whether the module count is the cause is not established: merging the one-entry
modules (audio, Venus, capture) keeps every cdhash and brings the count back down, so the next image tells.

A version 2 module is a little-endian u32 version (2), a 16-byte UUID, a u32 entry count, then entries of 24 bytes: the 20-byte
cdhash, its hash type, flags and two more bytes. Entries are kept as they are and sorted by cdhash; a cdhash in two inputs with
different trailing bytes is refused.
"""
import argparse
import hashlib
from pathlib import Path
import struct


def entries(path):
    data = path.read_bytes()
    version, count = struct.unpack_from('<I', data, 0)[0], struct.unpack_from('<I', data, 20)[0]
    if version != 2 or len(data) != 24 + 24 * count:
        raise SystemExit(f'{path}: not a version 2 trust cache module')
    return [data[24 + 24 * i:48 + 24 * i] for i in range(count)]


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('output', type=Path)
    parser.add_argument('inputs', type=Path, nargs='+')
    a = parser.parse_args()
    merged = {}
    for path in a.inputs:
        for entry in entries(path):
            if merged.setdefault(entry[:20], entry) != entry:
                raise SystemExit(f'{path}: cdhash {entry[:20].hex()} differs from another input')
    body = b''.join(merged[cdhash] for cdhash in sorted(merged))
    identity = hashlib.sha256(b'Coachz merged trust cache' + body).digest()[:16]
    a.output.write_bytes(struct.pack('<I16sI', 2, identity, len(merged)) + body)
    print(f'{a.output}: {len(merged)} entries from {len(a.inputs)} modules')


if __name__ == '__main__':
    main()
