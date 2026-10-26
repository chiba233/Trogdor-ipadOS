#!/usr/bin/env python3
"""Pack an instrumented, unslid source-XNU boot-args checkpoint; never boot it."""
import argparse
import hashlib
import json
from pathlib import Path
import struct

from xnu_macho import backed, macho

MAGIC = b'TRXNUC01'
HEADER = struct.Struct('<8sII14Q1024s')
WINDOW = 24 * 1024 * 1024
MMU_PREFIX_SHA256 = '446bb7c37baf4bb0e367ad4543779b42ef8c0d4757905e8aca57c6a9b7629571'
MMU_OFFSET = 0x2fc
HIGHVA_PREFIX_SHA256 = '311d82ebffc0036a13c350695ef04aea4082a161f280268a121106dc9d3f6183'
HIGHVA_PREFIX_BYTES = 0x330
STACK_LOAD = 0xf9403bf3
MMU_METADATA = struct.Struct('<6Q')
HVC = 0xd4000002 | (0x584e << 5)


def symbols(data, private_names=()):
    at, table = 32, None
    for _ in range(struct.unpack_from('<I', data, 16)[0]):
        command, size = struct.unpack_from('<II', data, at)
        if command == 2:
            if table is not None or size != 24:
                raise ValueError('ambiguous symbol table')
            table = struct.unpack_from('<4I', data, at + 8)
        at += size
    if table is None:
        raise ValueError('source symbols required')
    offset, count, strings, string_size = table
    if offset + count * 16 > len(data) or strings + string_size > len(data):
        raise ValueError('symbol table exceeds file')
    result = {}
    for index in range(count):
        name, kind, section, _, value = struct.unpack_from('<IBBHQ', data, offset + index * 16)
        if kind & 0xe != 0xe or kind & 0xe0 or (not kind & 1 and not private_names) or not section:
            continue
        if name >= string_size:
            raise ValueError('symbol string exceeds table')
        stop = data.find(b'\0', strings + name, strings + string_size)
        if stop < 0:
            raise ValueError('unterminated symbol')
        key = data[strings + name:stop].decode('ascii')
        if not kind & 1 and key not in private_names:
            continue
        if key in result and result[key] != value:
            raise ValueError('ambiguous defined symbol')
        result[key] = value
    return result


def adt_fields(data):
    at, found = 0, {}
    def visit(path, depth):
        nonlocal at
        if depth > 32 or at + 8 > len(data):
            raise ValueError('invalid ADT node bounds')
        count, children = struct.unpack_from('<II', data, at); at += 8
        props = {}
        for _ in range(count):
            if at + 36 > len(data):
                raise ValueError('truncated ADT property')
            name = data[at:at + 32]
            if b'\0' not in name:
                raise ValueError('unterminated ADT key')
            name = name.split(b'\0')[0].decode('ascii')
            size = struct.unpack_from('<I', data, at + 32)[0]; at += 36
            if size & 0x80000000 or size > len(data) - at or name in props:
                raise ValueError('invalid or duplicate ADT property')
            props[name] = (at, data[at:at + size]); at = (at + size + 3) & ~3
        raw = props.get('name', (0, b''))[1]
        if not raw.endswith(b'\0') or b'\0' in raw[:-1]:
            raise ValueError('invalid ADT node name')
        name = raw[:-1].decode('ascii')
        current = path + '/' + name if depth else ''
        if current in found:
            raise ValueError('duplicate ADT path')
        found[current] = props
        for _ in range(children): visit(current, depth + 1)
    if not 8 <= len(data) <= 1024 * 1024:
        raise ValueError('invalid ADT length')
    visit('', 0)
    if at != len(data): raise ValueError('trailing ADT bytes')
    for name in ('dram-base', 'dram-size'):
        if len(found['/chosen'][name][1]) != 8:
            raise ValueError('invalid ADT DRAM field')
    cpus = [p for path, p in found.items() if path.startswith('/cpus/')]
    if len(cpus) != 1 or len(cpus[0]['reg'][1]) != 4 or len(cpus[0]['timebase-frequency'][1]) != 4:
        raise ValueError('checkpoint requires one typed boot CPU')
    return (found['/chosen']['dram-base'][0], found['/chosen']['dram-size'][0],
            int.from_bytes(cpus[0]['reg'][1], 'little'),
            int.from_bytes(cpus[0]['timebase-frequency'][1], 'little'))


def pack(data, adt, command_line='-v debug=0xc cpus=1 keepsyms=1', mmu=False, highva=False):
    mmu = mmu or highva
    kernel, names = macho(data), symbols(data, ('_arm_init',) if highva else ())
    cold, pages = names['_start_first_cpu'], names['_bootstrap_pagetables']
    branch = struct.unpack_from('<I', data, backed(kernel.segments, kernel.entry, 4))[0]
    immediate = branch & 0x3ffffff
    if immediate & (1 << 25): immediate -= 1 << 26
    if branch >> 26 != 5 or kernel.entry + immediate * 4 != cold:
        raise ValueError('XNU entry no longer branches directly to cold start')
    offset = backed(kernel.segments, cold, 52)
    words = struct.unpack_from('<13I', data, offset)
    expected = {0: 0xd510109f, 1: 0xd5034fdf, 2: 0xaa0003f4, 3: 0xd2800015,
                5: 0x91000000, 6: 0xd518c000, 7: 0xf9400696, 8: 0xf9400a97,
                9: 0xf9400e98, 11: 0xf9423a9a, 12: 0xd51bd07f}
    if (any(words[i] != value for i, value in expected.items()) or
            words[4] & 0x9f00001f != 0x90000000 or words[10] & 0x9f00001f != 0x90000019):
        raise ValueError('unsupported XNU pre-MMU boot-args instruction prefix')
    def adrp_target(word, pc):
        immediate = ((word >> 5) & 0x7ffff) << 2 | (word >> 29) & 3
        if immediate & (1 << 20): immediate -= 1 << 21
        return (pc & ~4095) + immediate * 4096
    if adrp_target(words[10], cold + 40) != pages & ~4095:
        raise ValueError('bootstrap page-table symbol and instruction disagree')
    if struct.pack('<I', HVC) in data:
        raise ValueError('ambiguous preexisting checkpoint HVC')
    patch_va, version, metadata = cold + 48, 1, b''
    if mmu:
        prefix_at = backed(kernel.segments, cold, MMU_OFFSET + 4)
        if hashlib.sha256(data[prefix_at:prefix_at + MMU_OFFSET + 4]).hexdigest() != MMU_PREFIX_SHA256:
            raise ValueError('unsupported MMU cold-start code; requires independent instruction audit')
        helper = backed(kernel.segments, names['_set_mmu_ttb'], 16)
        if data[helper:helper+16] != bytes.fromhex('9f3b03d5002018d5df3f03d5c0035fd6'):
            raise ValueError('unsupported TTBR0 helper')
        if names['__mh_execute_header'] != kernel.base or struct.unpack_from('<I', data, 24)[0] & 0x80000000:
            raise ValueError('MMU checkpoint requires an ordinary standalone Mach header')
        if not any(va <= pages and pages + 65536 <= va + size and not prot & 4
                   for va, size, _, _, prot in kernel.segments):
            raise ValueError('bootstrap table pool is not inside non-executable kernel memory')
        tcr = struct.unpack_from('<Q', data, backed(kernel.segments, names['_sysreg_restore'], 8))[0]
        if tcr != 0x22a51a251a:
            raise ValueError('unsupported native 4K initial TCR')
        pointers = [names[key] for key in ('_ExceptionVectorsBase', '_intstack_top', '_excepstack_top')]
        if any(not kernel.base <= va < kernel.end or va & 15 for va in pointers):
            raise ValueError('invalid MMU vector/stack symbols')
        metadata = MMU_METADATA.pack(tcr, 0x0c0804ff00bb44ff, *pointers, 65536)
        patch_va, version = cold + MMU_OFFSET, 2
    patch_bytes = 4
    if highva:
        prefix_at = backed(kernel.segments, cold, HIGHVA_PREFIX_BYTES)
        if hashlib.sha256(data[prefix_at:prefix_at+HIGHVA_PREFIX_BYTES]).hexdigest() != HIGHVA_PREFIX_SHA256:
            raise ValueError('unsupported high-VA cold-start code; requires instruction audit')
        adrp, add = struct.unpack_from('<2I', data, prefix_at+0x68)
        c_entry = adrp_target(adrp, cold+0x68) + ((add>>10)&4095)
        if c_entry != names['_arm_init']:
            raise ValueError('C entry symbol and link-register instructions disagree')
        c_at = backed(kernel.segments, c_entry, 12)
        if struct.unpack_from('<3I', data, c_at) != (0xd10343ff, 0xa9076ffc, 0xa90867fa):
            raise ValueError('unsupported C entry prologue')
        stack_top = pointers[1]
        matches = [(va,size,off,length) for va,size,off,length,prot in kernel.segments
                   if not prot & 4 and va <= stack_top-0xd0 and stack_top <= va+size]
        if len(matches)!=1 or (stack_top-0xd0 < pages+65536 and pages < stack_top):
            raise ValueError('invalid high-VA stack frame or table-pool overlap')
        va,_,off,length=matches[0]
        slot=stack_top-0x60-va
        if any(data[off+slot+i] for i in range(8) if slot+i<length):
            raise ValueError('high-VA stack slot must load zero')
        patch_va, version, patch_bytes = c_entry+8, 3, 8
    patched = bytearray(data)
    patch_at = backed(kernel.segments, patch_va-(4 if highva else 0), patch_bytes)
    if highva:
        struct.pack_into('<2I', patched, patch_at, STACK_LOAD, HVC)
    else:
        struct.pack_into('<I', patched, patch_at, HVC)
    if macho(patched) != kernel:
        raise ValueError('checkpoint unexpectedly changes Mach-O metadata')
    base_field, size_field, affinity, frequency = adt_fields(adt)
    if not frequency: raise ValueError('zero ADT timebase')
    adt = bytearray(adt)
    for at in (base_field, size_field): adt[at:at + 8] = bytes(8)
    raw = command_line.encode('ascii')
    if len(raw) >= 1024 or b'\0' in raw: raise ValueError('invalid command line')
    table = b''.join(struct.pack('<5Q', va - kernel.base, size, off, length, prot)
                     for va, size, off, length, prot in kernel.segments)
    raw_at = (HEADER.size + len(table) + len(metadata) + 15) & ~15
    adt_at = (raw_at + len(patched) + 15) & ~15
    span = kernel.end - kernel.base
    bias = kernel.base & 0x1fffff if mmu else 0
    if ((span + bias + 0x3fff) & ~0x3fff) + 4096 + len(adt) + 0x4000 > WINDOW:
        raise ValueError('source kernel exceeds the bounded checkpoint arena')
    header = HEADER.pack(MAGIC, version, len(kernel.segments), kernel.base, kernel.entry,
        patch_va, pages & ~4095, raw_at, len(patched), adt_at, len(adt), span, WINDOW,
        base_field, size_field, affinity, frequency, raw)
    payload = header + table + metadata + bytes(raw_at - HEADER.size - len(table) - len(metadata)) + patched
    payload += bytes(adt_at - len(payload)) + adt
    return payload, {'kernel_sha256': hashlib.sha256(data).hexdigest(),
        'instrumented_kernel_sha256': hashlib.sha256(patched).hexdigest(),
        'local_relocations_audited': kernel.local_relocations,
        'patch_file_offset': patch_at, 'patch_bytes': patch_bytes, 'checkpoint_va': patch_va,
        'mmu_checkpoint': mmu, 'highva_checkpoint': highva, 'kernel_alignment_bias': bias,
        'kernel_span': span, 'arena_bytes': WINDOW, 'payload_sha256': hashlib.sha256(payload).hexdigest(),
        'hardware_boot_tested': False, 'prelinked': False}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--kernel', type=Path, required=True)
    p.add_argument('--kernel-report', type=Path, required=True)
    p.add_argument('--adt', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--mmu', action='store_true', help='checkpoint immediately after enabling MMU')
    p.add_argument('--highva', action='store_true', help='checkpoint at high-VA C entry after a stack read')
    a = p.parse_args()
    data = a.kernel.read_bytes()
    source = json.loads(a.kernel_report.read_text())
    if (not source.get('source_kernel_built') or source.get('prelinked') or
            source.get('xnu_version') != '10063.141.1' or
            source.get('source_kernel_sha256') != hashlib.sha256(data).hexdigest()):
        p.error('kernel must match the source-build report')
    sidecar = a.output.with_suffix('.json')
    if (a.output.resolve() == sidecar.resolve() or sidecar.exists() or
            a.output.exists() or a.output.resolve().is_relative_to(Path('/dev'))):
        p.error('output must be a new ordinary file')
    payload, report = pack(data, a.adt.read_bytes(), mmu=a.mmu, highva=a.highva)
    a.output.parent.mkdir(parents=True, exist_ok=True)
    a.output.write_bytes(payload)
    a.output.with_suffix('.json').write_text(json.dumps(report, indent=2) + '\n')
    print('Built source-XNU ' + ('high-VA' if a.highva else 'MMU-on' if a.mmu else 'pre-MMU') +
          ' checkpoint payload; no full XNU boot is implied.')


if __name__ == '__main__': main()
