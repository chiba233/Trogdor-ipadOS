#!/usr/bin/env python3
"""Inspect a private firmware FDT and prepare Darwin 23 handoff files on the host.

This is a planning tool. A selected RAM interval is not a firmware allocation,
and no file it produces is a bootable image or permission to branch into XNU.
"""
import argparse
from dataclasses import dataclass
import hashlib
import json
from pathlib import Path
import struct
import sys

from fdt import Node, decode, encode, string, u32
from read_probe import read_record

ROOT = Path(__file__).resolve().parents[1]
U64 = 1 << 64
BLOCK = 0x200000
PAGE = 0x4000  # Keep data placement compatible with both 4K and 16K alignment.
WINDOW = 0x10000000


def align(value, unit):
    return (value + unit - 1) // unit * unit


def interval(base, size):
    if not (0 <= base < U64 and 0 < size <= U64 - base):
        raise ValueError('empty or overflowing memory interval')
    return base, base + size


def overlap(a, b):
    return a[0] < b[1] and b[0] < a[1]


def cells(node, name, default):
    raw = node.props.get(name, struct.pack('>I', default))
    if len(raw) != 4 or int.from_bytes(raw, 'big') not in (0, 1, 2):
        raise ValueError('unsupported address/size cell width')
    return int.from_bytes(raw, 'big')


def ranges(raw, ac, sc):
    stride = 4 * (ac + sc)
    if not stride or not raw or len(raw) % stride:
        raise ValueError('invalid FDT register tuples')
    return [interval(int.from_bytes(raw[p:p + 4 * ac], 'big'),
                     int.from_bytes(raw[p + 4 * ac:p + stride], 'big'))
            for p in range(0, len(raw), stride)]


def live_tree(blob):
    # Only the exact pinned Depthcharge END-token quirk is normalized.
    if len(blob) >= 40:
        total, structure, strings_at, size = (struct.unpack_from('>I', blob, p)[0]
                                             for p in (4, 8, 12, 36))
        end = structure + size
        if total == len(blob) and end + 4 == strings_at and strings_at <= total and blob[end:end + 4] == b'\0\0\0\x09':
            fixed = bytearray(blob)
            struct.pack_into('>I', fixed, 36, size + 4)
            blob = bytes(fixed)
    return decode(blob)


def enabled(node):
    status = node.props.get('status')
    return status in (None, b'ok\0', b'okay\0')


def memory_map(tree):
    root = tree.root
    ac, sc = cells(root, '#address-cells', 2), cells(root, '#size-cells', 1)
    if not ac or not sc:
        raise ValueError('memory requires address and size cells')
    ram, excluded = [], [interval(a, n) for a, n in tree.reservations if n]
    for node in root.children:
        if not enabled(node):
            continue
        if node.props.get('device_type') == b'memory\0':
            ram.extend(ranges(node.props.get('reg', b''), ac, sc))
        if node.name == 'reserved-memory':
            if node.props.get('ranges') != b'':
                raise ValueError('reserved-memory requires explicit identity ranges')
            rac, rsc = cells(node, '#address-cells', ac), cells(node, '#size-cells', sc)
            for child in node.children:
                if not enabled(child):
                    continue
                if 'reg' in child.props:
                    excluded.extend(ranges(child.props['reg'], rac, rsc))
                elif 'size' in child.props:
                    if 'alloc-ranges' not in child.props:
                        raise ValueError('unbounded dynamic reserved-memory allocation')
                    # Conservatively exclude the whole possible allocation range.
                    excluded.extend(ranges(child.props['alloc-ranges'], rac, rsc))
                else:
                    raise ValueError('reserved-memory child has no known allocation')
        if node.name == 'chosen':
            start, end = (node.props.get('linux,initrd-' + key) for key in ('start', 'end'))
            if (start is None) != (end is None):
                raise ValueError('incomplete initrd range')
            if start is not None:
                if len(start) not in (4, 8) or len(start) != len(end):
                    raise ValueError('invalid initrd address width')
                first, last = int.from_bytes(start, 'big'), int.from_bytes(end, 'big')
                excluded.append(interval(first, last - first))
    ram.sort()
    if not ram or any(overlap(a, b) for a, b in zip(ram, ram[1:])):
        raise ValueError('missing or overlapping RAM banks')
    # Factory FIT/FDT/Depthcharge arenas must stay outside the candidate even
    # when the supplied tree does not list those live bootloader reservations.
    excluded += [interval(0xd0000000, 0x20000000), interval(0xf0000000, 0x10000000)]
    return ram, sorted(excluded)


def select_window(ram, excluded, size=WINDOW):
    if size < WINDOW or size > 0x100000000 or size % BLOCK:
        raise ValueError('kernel window must be 256 MiB..4 GiB in 2 MiB steps')
    # Prefer upper RAM; never span banks or write an address discovered here.
    for lo, hi in sorted(ram, key=lambda pair: (pair[0] < 0x100000000, pair[0])):
        candidate = align(lo, BLOCK)
        for first, last in sorted(excluded):
            if last <= candidate:
                continue
            if candidate + size <= min(first, hi):
                return candidate, size
            if first < candidate + size:
                candidate = align(max(candidate, last), BLOCK)
            if candidate + size > hi:
                break
        if candidate + size <= hi:
            return candidate, size
    raise ValueError('no single contiguous RAM window avoids all exclusions')


@dataclass(frozen=True)
class Kernel:
    base: int
    end: int
    entry: int
    segments: tuple  # vmaddr, vmsize, fileoff, filesize, initial protection


def macho(data):
    if not 32 <= len(data) <= 128 * 1024 * 1024:
        raise ValueError('invalid Mach-O file size')
    magic, cpu, subtype, kind, count, size, flags, reserved = struct.unpack_from('<8I', data)
    if (magic, cpu, subtype, kind) != (0xfeedfacf, 0x100000c, 0, 2) or reserved:
        raise ValueError('requires raw ARM64-all MH_EXECUTE; ARM64e/FAT/IMG4 are unsupported')
    if size > len(data) - 32 or count > size // 8:
        raise ValueError('invalid Mach-O command bounds')
    end, at, segments, entry = 32 + size, 32, [], None
    for _ in range(count):
        if at + 8 > end:
            raise ValueError('truncated Mach-O command')
        command, length = struct.unpack_from('<II', data, at)
        if length < 8 or length % 8 or length > end - at:
            raise ValueError('invalid Mach-O command length')
        if command == 0x19:
            if length < 72:
                raise ValueError('truncated segment command')
            va, memory, offset, file_size, maximum, protection, nsects, seg_flags = struct.unpack_from('<4Q4I', data, at + 24)
            if seg_flags & ~0x14:  # SG_NORELOC / SG_READ_ONLY preserve ordinary placement.
                raise ValueError('unsupported Mach-O segment flags')
            if length != 72 + 80 * nsects or len(segments) >= 64:
                raise ValueError('invalid segment section count')
            if memory:
                interval(va, memory)
                if memory > WINDOW or file_size > memory or offset > len(data) or file_size > len(data) - offset:
                    raise ValueError('invalid segment memory/file bounds')
                if protection & ~maximum or maximum & ~7:
                    raise ValueError('invalid segment protection')
                segments.append((va, memory, offset, file_size, protection))
            elif file_size:
                raise ValueError('file data without segment memory')
            for section in range(nsects):
                if struct.unpack_from('<I', data, at + 72 + section * 80 + 60)[0]:
                    raise ValueError('unresolved section relocations')
        elif command == 5:
            if entry is not None or length != 288 or struct.unpack_from('<II', data, at + 8) != (6, 68):
                raise ValueError('requires one ARM_THREAD_STATE64 LC_UNIXTHREAD')
            entry = struct.unpack_from('<Q', data, at + 272)[0]
        elif command in (0x80000034, 0x80000022, 0x22, 0x80000028):
            raise ValueError('unresolved dyld fixups or LC_MAIN are unsupported; supply a qualified prelinked kernel')
        elif command == 0xb:
            if length != 80 or any(struct.unpack_from('<I', data, at + n)[0] for n in (28, 68, 76)):
                raise ValueError('unresolved Mach-O symbols or relocations')
        elif command not in (2, 0x1b, 0x1d, 0x26, 0x29, 0x2a, 0x32):
            raise ValueError('unsupported Mach-O command')
        at += length
    if at != end or not segments or entry is None or entry % 4:
        raise ValueError('missing segments/entry or noncanonical commands')
    segments.sort()
    if any(a[0] + a[1] > b[0] for a, b in zip(segments, segments[1:])):
        raise ValueError('overlapping virtual segments')
    base, top = segments[0][0], max(s[0] + s[1] for s in segments)
    if base % 4096 or top - base > WINDOW - BLOCK or base > U64 - WINDOW:
        raise ValueError('invalid kernel virtual window')
    if not any(s[4] & 4 and s[0] <= entry and entry + 4 <= s[0] + s[3] for s in segments):
        raise ValueError('entry is not in file-backed executable memory')
    return Kernel(base, top, entry, tuple(segments))


def boot_args(kernel, physical, memory, adt_size, command_line):
    interval(physical, memory)
    if physical % BLOCK or memory < WINDOW or memory % BLOCK or memory > 0x100000000:
        raise ValueError('invalid physical window')
    raw = command_line.encode('ascii')
    if b'\0' in raw or len(raw) >= 1024 or not 0 < adt_size <= 0x1000000:
        raise ValueError('invalid Darwin 23 command line or ADT size')
    args_pa = align(physical + kernel.end - kernel.base, PAGE)
    tree_pa = args_pa + 4096
    top = align(tree_pa + adt_size, PAGE)
    if top > physical + memory - BLOCK or kernel.base + memory > U64:
        raise ValueError('handoff data exceeds kernel window')
    result = bytearray(0x480)
    struct.pack_into('<HH', result, 0, 2, 2)
    struct.pack_into('<4Q', result, 8, kernel.base, physical, memory, top)
    struct.pack_into('<QI', result, 0x60, kernel.base + tree_pa - physical, adt_size)
    result[0x6c:0x6c + len(raw)] = raw
    struct.pack_into('<QQ', result, 0x470, 0, memory)
    return bytes(result), {'boot_args_pa': args_pa, 'adt_pa': tree_pa,
                           'top_of_kernel_data': top, 'entry_pa': physical + kernel.entry - kernel.base}


def hardware_tree(tree, fields, physical, size):
    """Construct the small typed hardware input for upstream's common ADT generator."""
    root = tree.root
    compatible = root.props.get('compatible', b'')
    if not compatible.endswith(b'\0') or b'qcom,sc7180' not in compatible.split(b'\0'):
        raise ValueError('hardware input is not an SC7180 tree')
    cpus = next((c for c in root.children if c.name == 'cpus'), None)
    if cpus is None:
        raise ValueError('missing CPU tree')
    ac = cells(cpus, '#address-cells', 2)
    if not ac:
        raise ValueError('CPU affinity requires address cells')
    affinity = fields['MPIDR'] & 0xff00ffffff
    candidates = [n for n in cpus.children if n.props.get('device_type') == b'cpu\0' and
                  len(n.props.get('reg', b'')) == ac * 4 and
                  int.from_bytes(n.props['reg'], 'big') == affinity and enabled(n)]
    if len(candidates) != 1:
        raise ValueError('boot MPIDR does not uniquely match an enabled FDT CPU')
    controllers = []
    def visit(node):
        if not enabled(node):
            return
        if b'arm,gic-v3' in node.props.get('compatible', b'').split(b'\0'):
            controllers.append(node)
        for child in node.children:
            visit(child)
    visit(root)
    if len(controllers) != 1:
        raise ValueError('requires exactly one GICv3 controller')
    gic = controllers[0]
    # Decode the actual parent ranges; Coachz uses an explicit zero-to-zero
    # mapping, not an empty ranges property. Nested buses need separate work.
    soc = next((c for c in root.children if gic in c.children), None)
    if soc is None:
        raise ValueError('unsupported GIC parent bus')
    ac, sc = cells(soc, '#address-cells', 2), cells(soc, '#size-cells', 1)
    pac = cells(root, '#address-cells', 2)
    raw = soc.props.get('ranges')
    if raw is None:
        raise ValueError('GIC bus has no ranges mapping')
    stride = 4 * (ac + pac + sc)
    if not ac or not pac or not sc or len(raw) % stride:
        raise ValueError('invalid GIC bus ranges')
    mappings = []
    for at in range(0, len(raw), stride):
        bus = int.from_bytes(raw[at:at + ac * 4], 'big')
        parent = int.from_bytes(raw[at + ac * 4:at + (ac + pac) * 4], 'big')
        size_bytes = int.from_bytes(raw[at + (ac + pac) * 4:at + stride], 'big')
        mappings.append((interval(bus, size_bytes), interval(parent, size_bytes)))
    gic_regs = ranges(gic.props.get('reg', b''), ac, sc)
    translated = []
    for lo, hi in gic_regs:
        matches = [p[0] + lo - c[0] for c, p in mappings if c[0] <= lo and hi <= c[1]] if raw else [lo]
        if len(matches) != 1:
            raise ValueError('missing or ambiguous GIC bus translation')
        translated.append(interval(matches[0], hi - lo))
    if len(translated) < 2:
        raise ValueError('GIC distributor/redistributor ranges are missing')
    props = {key: gic.props[key] for key in ('compatible', 'redistributor-stride') if key in gic.props}
    props.update({'reg': b''.join(struct.pack('>QQ', lo, hi - lo) for lo, hi in translated),
                  'unixnu,adt-name': string('intc'), 'interrupt-controller': b'', '#interrupt-cells': u32(3)})
    cpu = candidates[0]
    return encode(Node('', {'#address-cells': u32(2), '#size-cells': u32(2),
        'model': string('Google Coachz'), 'compatible': string('google,coachz') + string('qcom,sc7180')}, [
        Node('memory@' + format(physical, 'x'), {'device_type': string('memory'), 'reg': struct.pack('>QQ', physical, size)}),
        Node('cpus', {'#address-cells': u32(2), '#size-cells': u32(0), 'timebase-frequency': u32(fields['CNTFRQ'])}, [
            Node('cpu@' + format(affinity, 'x'), {'device_type': string('cpu'), 'compatible': cpu.props['compatible'],
                 'reg': affinity.to_bytes(8, 'big')})]),
        Node('soc', {'#address-cells': u32(2), '#size-cells': u32(2), 'ranges': b''}, [Node('interrupt-controller', props)])]))


def probe_fields(raw):
    text, fields = read_record(raw)
    for key in ('CNTFRQ', 'MPIDR'):
        values = [line.split('=', 1)[1] for line in text.splitlines() if line.startswith(key + '=')]
        if len(values) != 1 or len(values[0]) != 18 or not values[0].startswith('0x'):
            raise ValueError('missing or ambiguous probe register: ' + key)
        fields[key] = int(values[0], 16)
    if not 0 < fields['CNTFRQ'] <= 0xffffffff:
        raise ValueError('invalid timebase frequency')
    return fields


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--dtb', type=Path, required=True)
    p.add_argument('--probe-log', type=Path, required=True)
    p.add_argument('--kernel', type=Path, help='optional qualified prelinked raw Mach-O; never executed')
    p.add_argument('--cmdline', default='-v debug=0xc cpus=1 keepsyms=1')
    p.add_argument('--output', type=Path, default=ROOT / 'build/xnu-handoff')
    a = p.parse_args()
    out = a.output.resolve()
    if out.exists() or out.is_relative_to(Path('/dev')):
        p.error('output must be a new ordinary directory outside /dev')
    from kernel_source import verify
    base = verify('unixnu')
    tree = live_tree(a.dtb.read_bytes())
    fields = probe_fields(a.probe_log.read_bytes())
    ram, excluded = memory_map(tree)
    physical, size = select_window(ram, excluded)
    hardware = hardware_tree(tree, fields, physical, size)
    # Import only the pinned shared ADT generator; no upstream CLI/build is run.
    sys.path.insert(0, str(base / 'scripts'))
    from unixnu.devicetree import generate
    out.mkdir(parents=True)
    hardware_path = out / 'hardware.dtb'
    hardware_path.write_bytes(hardware)
    adt = generate(hardware_path, {'adt': {'chosen': {'string': {'osenvironment': 'ramdisk'}},
                     'defaults': {'u32': {'sleep-disabled': 1}}}})
    (out / 'kernel.macho.adt').write_bytes(adt)
    report = {'schema': 1, 'darwin': '23.6.0', 'boot_args_bytes': 0x480,
              'ram_banks': ram, 'excluded_ranges': excluded, 'candidate_base': physical,
              'candidate_size': size, 'boot_cpu_affinity': fields['MPIDR'] & 0xff00ffffff,
              'timebase_frequency': fields['CNTFRQ'], 'cpu_count': 1,
              'adt_bytes': len(adt), 'adt_sha256': hashlib.sha256(adt).hexdigest(),
              'firmware_allocation_performed': False, 'kernel_loaded': False,
              'hardware_boot_tested': False, 'kernel_supplied': bool(a.kernel)}
    if a.kernel:
        blob = a.kernel.read_bytes()
        kernel = macho(blob)
        args, layout = boot_args(kernel, physical, size, len(adt), a.cmdline)
        (out / 'boot_args.bin').write_bytes(args)
        report.update(layout, kernel_sha256=hashlib.sha256(blob).hexdigest(),
                      virtual_base=kernel.base, segments=kernel.segments)
    (out / 'manifest.json').write_text(json.dumps(report, indent=2) + '\n')
    print('Host handoff preparation complete:', out)
    print('Candidate RAM only; no firmware allocation, kernel execution or device write.')


if __name__ == '__main__':
    main()
