#!/usr/bin/env python3
"""Pack a source-XNU screen-output payload: no checkpoint patch, no return to Linux."""
import argparse
import hashlib
from pathlib import Path
import re
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
# The Cryptex1,SystemOS volume of the pinned IPSW, mounted by coachz-prelink-systemos.patch.
SYSTEMOS_UUID = '8846FE78-2910-4297-AF97-D59BB6F8FC16'
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


def wrap_trust_cache(modules):
    """The memory-map segment: a module count and offset table in front of the original modules."""
    if isinstance(modules, bytes):
        modules = [modules]
    offsets, at = [], 4 + 4 * len(modules)
    for module in modules:
        offsets.append(at)
        at += len(module)
    return struct.pack('<%dI' % (1 + len(modules)), len(modules), *offsets) + b''.join(modules)


def root_matching(uuid):
    """/chosen/root-matching: IOFindBSDRoot takes it before rd=, matching the APFS volume media by UUID.
    rd=uuid would wait for a boot-uuid-media resource that only a file system driver kext publishes."""
    return ('<dict><key>IOProviderClass</key><string>IOMedia</string><key>IOPropertyMatch</key>'
            '<dict><key>UUID</key><string>%s</string></dict></dict>' % uuid).encode() + b'\0'


def prelink_tree(adt, window, tree, ramdisk, cache, environment=None, ram_root=True, root_uuid=None):
    root = read_tree(adt)
    table = tomllib.loads(OS_PROFILE.read_text())['adt']
    apply_identity(root, table['root'])
    for name in ('chosen', 'product', 'defaults'):
        apply_identity(root.child(name), table[name])
    if environment:
        root.child('chosen').set('osenvironment', environment.encode() + b'\0')
    if root_uuid:
        root.child('chosen').set('root-matching', root_matching(root_uuid))
        # mount -P 1 of the original launchd exits 66 without it. An identity of this unsigned image set,
        # not a measurement: nothing here is verified.
        root.child('chosen').set('boot-manifest-hash', hashlib.sha384(
            b'Coachz unsealed USB boot set\0' + root_uuid.encode() + b'\0' + SYSTEMOS_UUID.encode()).digest())
    memory_map = root.child('chosen').child('memory-map')
    # Without a RAMDisk entry IOFindBSDRoot makes no md0, and rd= names a disk partition.
    if ram_root:
        memory_map.set('RAMDisk', bytes(16))
    memory_map.set('TrustCache', bytes(16))
    disk = window + up(tree + len(root.write()))
    if ram_root:
        memory_map.set('RAMDisk', struct.pack('<QQ', disk, len(ramdisk)))
    memory_map.set('TrustCache', struct.pack('<QQ', disk + len(ramdisk), len(cache)))
    return root.write()


def pack(data, adt, window, memory, command_line=None, ramdisk=None, prelinked=False, root_cache=None, root_device=None,
         root_uuid=None, data_seed=None):
    if window & 0x1fffff or memory & 0x1fffff:
        raise ValueError('window base and size must be 2 MiB aligned')
    kernel = macho(data)
    disk_root = root_device or root_uuid
    if disk_root and (not prelinked or not root_cache or ramdisk is not None or command_line or (root_device and root_uuid)):
        raise ValueError('a disk root needs the prelinked kernel, its trust cache and no RAM disk')
    command_line = command_line or (PRELINK_COMMAND_LINE if prelinked else COMMAND_LINE)
    if root_device:
        if not re.fullmatch(r'disk[0-9]+(s[0-9]+)*', root_device):
            raise ValueError('invalid root device name')
        command_line = command_line.replace('rd=md0', 'rd=' + root_device)
    if root_uuid:
        # The main OS volume by its APFS volume UUID: container numbering does not matter.
        if not re.fullmatch(r'[0-9A-F]{8}(-[0-9A-F]{4}){3}-[0-9A-F]{12}', root_uuid):
            raise ValueError('invalid root volume UUID')
        command_line = command_line.replace(' rd=md0', '')
    if data_seed is not None and not root_uuid:
        raise ValueError('a Data seed goes with the main OS root')
    if data_seed:
        # The real board has no IOWatchdog service yet; the override lives only in memory Data.
        command_line += ' coachz-disable-watchdogd=1'
    if disk_root:
        # The main OS can carry its Data seed as the RAM disk: md0, mounted on /private/var by the kernel.
        ramdisk = data_seed or b''
    ramdisk = init_ramdisk() if ramdisk is None else ramdisk
    span = kernel.end - kernel.base
    bias = kernel.base & 0x1fffff
    # Same placement as boot/xnu_screen.c: kernel, boot_args page, ADT, RAM disk.
    tree = up(span + bias) + 4096
    if prelinked:
        cache = wrap_trust_cache(root_cache) if root_cache else trust_cache(ramdisk)
        ramdisk += bytes(-len(ramdisk) % 4096)
        # The restore volume, from memory or from disk, runs as the restore OS environment; the main OS does not.
        environment = 'restore' if root_cache and not root_uuid else None
        adt = prelink_tree(adt, window, tree, ramdisk, cache, environment, ram_root=not disk_root or bool(data_seed),
                           root_uuid=root_uuid)
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
    p.add_argument('--root-trust-cache', type=Path, action='append', default=[],
                   help='an original trust cache module of the root; repeat for the main OS and its cryptexes')
    p.add_argument('--root-device', help='with --prelinked: root on this BSD disk instead of a RAM disk, such as disk1s1')
    p.add_argument('--root-uuid', help='with --prelinked: the main OS, rooted on the APFS volume of this UUID')
    p.add_argument('--data-image', type=Path, help='with --root-uuid: APFS Data seed, the RAM disk the kernel mounts on /private/var')
    a = p.parse_args()
    if a.output.exists() or a.output.resolve().is_relative_to(Path('/dev')):
        p.error('output must be a new ordinary file')
    if a.root_device or a.root_uuid:
        if bool(a.root_device) == bool(a.root_uuid) or a.root_image or not a.root_trust_cache or not a.prelinked:
            p.error('--root-device or --root-uuid takes --root-trust-cache and --prelinked, and no --root-image')
    elif bool(a.root_image) != bool(a.root_trust_cache) or (a.root_image and not a.prelinked) or len(a.root_trust_cache) > 1:
        p.error('--root-image and one --root-trust-cache go together and need --prelinked')
    payload = pack(a.kernel.read_bytes(), a.adt.read_bytes(), a.window, a.memory, prelinked=a.prelinked,
                   ramdisk=a.root_image.read_bytes() if a.root_image else None,
                   root_cache=[cache.read_bytes() for cache in a.root_trust_cache] or None, root_device=a.root_device,
                   root_uuid=a.root_uuid, data_seed=a.data_image.read_bytes() if a.data_image else None)
    a.output.parent.mkdir(parents=True, exist_ok=True)
    a.output.write_bytes(payload)
    print('Built screen-output XNU payload', hashlib.sha256(payload).hexdigest())


if __name__ == '__main__':
    main()
