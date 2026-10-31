#!/usr/bin/env python3
"""Pack a source-XNU screen-output payload: no checkpoint patch, no return to Linux."""
import argparse
import hashlib
from pathlib import Path
import struct
import tomllib

from xnu_checkpoint import HEADER, MAGIC, adt_fields
from xnu_init import ramdisk as init_ramdisk, trust_cache
from xnu_macho import macho

VERSION = 12
# Without prelinked components the type distribution is skewed: the default budget of
# 260 gives one size class more than the 32 zones kalloc allows.
COMMAND_LINE = '-v debug=0xc serial=3 cpus=1 keepsyms=1 -enable_kprintf_spam kt_fixed_zones=120 rd=md0 cs_enforcement_disable=1'
# Original AMFI runs in the prelinked kernel: the init program is trusted through the static trust cache.
PRELINK_COMMAND_LINE = '-v debug=0xc serial=3 cpus=1 keepsyms=1 -enable_kprintf_spam rd=md0'
OS_PROFILE = Path(__file__).resolve().parents[1] / 'upstream/unixnu/profiles/os/ipados-17.7.11-21H461/profile.toml'


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


class Node:
    """An ADT node: ordered (key, raw length word, value) properties and children."""
    def __init__(self):
        self.props, self.children = [], []

    def name(self):
        return next((value.split(b'\0')[0].decode() for key, _, value in self.props if key == 'name'), '')

    def set(self, key, value):
        self.props = [p for p in self.props if p[0] != key] + [(key, len(value), value)]

    def child(self, name):
        for node in self.children:
            if node.name() == name:
                return node
        node = Node()
        node.set('name', name.encode() + b'\0')
        self.children.append(node)
        return node

    def write(self):
        out = struct.pack('<II', len(self.props), len(self.children))
        for key, length, value in self.props:
            out += key.encode().ljust(32, b'\0') + struct.pack('<I', length) + value + bytes(-len(value) % 4)
        return out + b''.join(node.write() for node in self.children)


def read_tree(adt):
    at = 0
    def visit():
        nonlocal at
        node = Node()
        props, children = struct.unpack_from('<II', adt, at)
        at += 8
        for _ in range(props):
            key = adt[at:at + 32].split(b'\0')[0].decode()
            length = struct.unpack_from('<I', adt, at + 32)[0]
            size = length & 0x7fffffff
            node.props.append((key, length, bytes(adt[at + 36:at + 36 + size])))
            at = (at + 36 + size + 3) & ~3
        node.children = [visit() for _ in range(children)]
        return node
    return visit()


def apply_identity(node, table):
    """Apply a typed [adt] table of the OS profile: what the original components read."""
    for kind, values in table.items():
        if kind == 'string':
            for key, value in values.items():
                node.set(key, value.encode() + b'\0')
        elif kind == 'u32':
            for key, value in values.items():
                node.set(key, struct.pack('<I', value))
        elif kind == 'bytes':
            for key, value in values.items():
                node.set(key, bytes.fromhex(value))
        else:
            apply_identity(node.child(kind), values)


def wrap_trust_cache(module):
    """The memory-map segment: an offset table in front of one original module."""
    return struct.pack('<II', 1, 8) + module


def prelink_tree(adt, window, tree, ramdisk, cache, environment=None):
    root = read_tree(adt)
    table = tomllib.loads(OS_PROFILE.read_text())['adt']
    apply_identity(root, table['root'])
    for name in ('chosen', 'product', 'defaults'):
        apply_identity(root.child(name), table[name])
    if environment:
        root.child('chosen').set('osenvironment', environment.encode() + b'\0')
    memory_map = root.child('chosen').child('memory-map')
    memory_map.set('RAMDisk', bytes(16))
    memory_map.set('TrustCache', bytes(16))
    disk = window + up(tree + len(root.write()))
    memory_map.set('RAMDisk', struct.pack('<QQ', disk, len(ramdisk)))
    memory_map.set('TrustCache', struct.pack('<QQ', disk + len(ramdisk), len(cache)))
    return root.write()


def pack(data, adt, window, memory, command_line=None, ramdisk=None, prelinked=False, root_cache=None):
    if window & 0x1fffff or memory & 0x1fffff:
        raise ValueError('window base and size must be 2 MiB aligned')
    kernel = macho(data)
    command_line = command_line or (PRELINK_COMMAND_LINE if prelinked else COMMAND_LINE)
    ramdisk = init_ramdisk() if ramdisk is None else ramdisk
    span = kernel.end - kernel.base
    bias = kernel.base & 0x1fffff
    # Same placement as boot/xnu_screen.c: kernel, boot_args page, ADT, RAM disk.
    tree = up(span + bias) + 4096
    if prelinked:
        cache = wrap_trust_cache(root_cache) if root_cache else trust_cache(ramdisk)
        ramdisk += bytes(-len(ramdisk) % 4096)
        adt = prelink_tree(adt, window, tree, ramdisk, cache, 'restore' if root_cache else None)
        ramdisk = ramdisk + cache + bytes(-len(cache) % 4096)
    else:
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
    # Aligned so that the loader can copy in 64-bit units: byte by byte takes minutes with the caches off.
    ramdisk_at = (adt_at + len(adt) + 15) & ~15
    header = HEADER.pack(MAGIC, VERSION, len(kernel.segments), kernel.base, kernel.entry, window, ramdisk_at, raw_at, len(data),
                         adt_at, len(adt), span, memory, base_field, size_field, affinity, frequency, raw)
    payload = header + table + bytes(raw_at - HEADER.size - len(table)) + data
    return payload + bytes(adt_at - len(payload)) + adt + bytes(ramdisk_at - adt_at - len(adt)) + ramdisk


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--kernel', type=Path, required=True)
    p.add_argument('--adt', type=Path, required=True)
    p.add_argument('--window', type=lambda v: int(v, 0), required=True, help='physical base; choose it from the private device tree')
    p.add_argument('--memory', type=lambda v: int(v, 0), default=1 << 30)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--prelinked', action='store_true', help='kernel carries the original components: trust the init program through a trust cache')
    p.add_argument('--root-image', type=Path, help='with --prelinked: RAM disk root instead of the init program; from tools/xnu_ipsw.py')
    p.add_argument('--root-trust-cache', type=Path, help='the original trust cache module of --root-image')
    a = p.parse_args()
    if a.output.exists() or a.output.resolve().is_relative_to(Path('/dev')):
        p.error('output must be a new ordinary file')
    if bool(a.root_image) != bool(a.root_trust_cache) or (a.root_image and not a.prelinked):
        p.error('--root-image and --root-trust-cache go together and need --prelinked')
    payload = pack(a.kernel.read_bytes(), a.adt.read_bytes(), a.window, a.memory, prelinked=a.prelinked,
                   ramdisk=a.root_image.read_bytes() if a.root_image else None,
                   root_cache=a.root_trust_cache.read_bytes() if a.root_trust_cache else None)
    a.output.parent.mkdir(parents=True, exist_ok=True)
    a.output.write_bytes(payload)
    print('Built screen-output XNU payload', hashlib.sha256(payload).hexdigest())


if __name__ == '__main__':
    main()
