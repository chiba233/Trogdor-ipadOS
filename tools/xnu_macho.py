"""Bounded ARM64 Mach-O inspection; local pointers are accepted only at VA slide zero."""
from dataclasses import dataclass
import struct

U64 = 1 << 64
WINDOW = 0x10000000
BLOCK = 0x200000


def interval(base, size):
    if not (0 <= base < U64 and 0 < size <= U64 - base):
        raise ValueError('invalid address interval')
    return base, base + size


def backed(segments, address, size):
    matches = [s[2] + address - s[0] for s in segments
               if s[0] <= address and size <= s[3] and address - s[0] <= s[3] - size]
    if len(matches) != 1:
        raise ValueError('address is not uniquely file backed')
    return matches[0]


def audit_local(data, table, segments, sections, origin):
    offset, count = table
    if offset > len(data) or count > (len(data) - offset) // 8:
        raise ValueError('local relocation table exceeds file')
    if not any(s[2] <= offset and offset + count * 8 <= s[2] + s[3] for s in segments):
        raise ValueError('local relocation metadata is not file backed')
    sites = []
    for index in range(count):
        relative, flags = struct.unpack_from('<iI', data, offset + index * 8)
        ordinal = flags & 0xffffff
        # ARM64_RELOC_UNSIGNED, local, 64-bit, non-PC-relative, section ordinal.
        if flags & 0xff000000 != 0x06000000 or not 1 <= ordinal <= len(sections) or relative < 0:
            raise ValueError('unsupported local pointer relocation')
        address = origin + relative
        site = backed(segments, address, 8)
        target = struct.unpack_from('<Q', data, site)[0]
        section, extent = sections[ordinal - 1]
        if not section <= target <= section + extent or not any(s[0] <= target <= s[0] + s[1] for s in segments):
            raise ValueError('linked pointer disagrees with relocation target section')
        sites.append(address)
    sites.sort()
    if any(a + 8 > b for a, b in zip(sites, sites[1:])):
        raise ValueError('overlapping or duplicate relocation sites')
    return count


@dataclass(frozen=True)
class Kernel:
    base: int
    end: int
    entry: int
    segments: tuple  # vmaddr, vmsize, fileoff, filesize, initial protection
    local_relocations: int = 0


def macho(data):
    if not 32 <= len(data) <= 128 * 1024 * 1024:
        raise ValueError('invalid Mach-O file size')
    magic, cpu, subtype, kind, count, size, flags, reserved = struct.unpack_from('<8I', data)
    if (magic, cpu, subtype, kind) != (0xfeedfacf, 0x100000c, 0, 2) or reserved:
        raise ValueError('requires raw ARM64-all MH_EXECUTE; ARM64e/FAT/IMG4 are unsupported')
    if size > len(data) - 32 or count > size // 8:
        raise ValueError('invalid Mach-O command bounds')
    end, at, segments, entry = 32 + size, 32, [], None
    sections, local, first_segment = [], None, None
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
            if first_segment is None:
                first_segment = va
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
                position = at + 72 + section * 80
                address, extent = struct.unpack_from('<QQ', data, position + 32)
                if address < va or address + extent > va + memory:
                    raise ValueError('section exceeds segment memory')
                sections.append((address, extent))
                if struct.unpack_from('<I', data, position + 60)[0]:
                    raise ValueError('unresolved section relocations')
        elif command == 5:
            if entry is not None or length != 288 or struct.unpack_from('<II', data, at + 8) != (6, 68):
                raise ValueError('requires one ARM_THREAD_STATE64 LC_UNIXTHREAD')
            entry = struct.unpack_from('<Q', data, at + 272)[0]
        elif command in (0x80000034, 0x80000022, 0x22, 0x80000028):
            raise ValueError('unresolved dyld fixups or LC_MAIN are unsupported; supply a qualified prelinked kernel')
        elif command == 0xb:
            if local is not None or length != 80 or any(struct.unpack_from('<I', data, at + n)[0] for n in (28, 68)):
                raise ValueError('unresolved Mach-O symbols/external relocations')
            local = struct.unpack_from('<II', data, at + 72)
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
    count = 0
    if local is not None and local[1]:
        if flags & 0x20 or first_segment != base:  # MH_SPLIT_SEGS uses a different origin.
            raise ValueError('unsupported local relocation origin')
        count = audit_local(data, local, segments, sections, first_segment)
    return Kernel(base, top, entry, tuple(segments), count)
