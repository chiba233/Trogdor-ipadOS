#!/usr/bin/env python3
"""Record the original IONetworkingFamily classes that the Wi-Fi registry subclasses.

The Apple USB host stack prelinks the original IONetworkingFamily (AppleUSBXHCI -> IOAccessoryManager -> AppleOnboardSerial ->
IOSkywalkFamily -> IONetworkingFamily, and every USB network driver), whose IONetworkInterface and IONetworkStack names the Wi-Fi
registry used to define itself. CoachzWifiNet.cpp therefore derives from the original classes. This finds, in the pinned original
kernelcache, each class's metaclass registration, its one dispatch table, protected constructor and base destructor by instruction
shape, checks the slot count and the IOService slots the original overrides, and writes the declarations and the addresses at the OS
profile slide, as tools/record_storage_bindings.py does for IOBlockStorageDevice. The written files hold addresses, sizes and slot
numbers only.

Usage: record_network_bindings.py <original kernelcache.decompressed> [--slide -0x1000000] [--output DIR]
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'upstream/unixnu/kits/XnuPrelink'))
import native_prelink as np  # noqa: E402

ORIGINAL_SHA256 = 'f18b4df865405f2a287b84977d7aca745e056d656d9fc9df1662970ce2969de7'
FAMILY = 'com.apple.iokit.IONetworkingFamily'
FAMILY_TEXT_SHA256 = '84482be879cb943c843b7525520a85ce9e58527033fb3b869cbe96a7186fa76d'
IOSERVICE_SLOTS = 168
BASES = {'IOService': 168, 'IOUserClient': 186}
# name: (base, instance size, slots, pure slots, base slots the original overrides, bound). A bound class is subclassed in C++ and its
# code is linked by address; the other is allocated by name at run time (its class name is what a sandbox checks) and gets a copy of
# its own table with every override but the destructors and getMetaClass replaced, so its slots are recorded and nothing is bound.
CLASSES = {
    'IONetworkInterface': ('IOService', 0x110, 221, [178], [0, 1, 7, 14, 43, 75, 76, 91, 92, 93, 132, 141, 159, 160], True),
    'IONetworkStack': ('IOService', 0xd0, 168, [], [0, 1, 7, 14, 44, 77, 86, 87, 95], True),
    'IONetworkStackUserClient': ('IOUserClient', 0xe0, 186, [], [0, 1, 7, 44, 86, 170, 172, 173], False),
}
OUTPUT = ROOT / 'platform/coachz-prelink/package'


def require(value, message):
    if not value:
        raise SystemExit('record_network_bindings: ' + message)


def page(pc, word):
    immediate = ((word >> 5) & 0x7ffff) << 2 | ((word >> 29) & 3)
    if immediate & (1 << 20):
        immediate -= 1 << 21
    return (pc & ~4095) + (immediate << 12)


def branch(pc, word):
    immediate = word & 0x3ffffff
    return pc + 4 * (immediate - (1 << 26) if immediate & (1 << 25) else immediate)


def is_adrp(word, register):
    return word & 0x9f00001f == 0x90000000 | register


def is_add(word, register):
    return word & 0xffc003ff == 0x91000000 | register << 5 | register


def is_bl(word):
    return word & 0xfc000000 == 0x94000000


def is_b(word):
    return word & 0xfc000000 == 0x14000000


def mangled(cls, method):
    return f'__ZN{len(cls)}{cls}{len(method)}{method}Ev'


def record(kernelcache, slide):
    data = kernelcache.read_bytes()
    require(hashlib.sha256(data).hexdigest() == ORIGINAL_SHA256, 'unexpected original kernelcache')
    top = np.macho(data)
    segments = top['segments']

    def read(va, length):
        at = np.backed_offset(segments, va, length, len(data))
        return data[at:at + length]

    def word(va):
        return struct.unpack('<I', read(va, 4))[0]

    def qword(va):
        return struct.unpack('<Q', read(va, 8))[0]

    info = next(s for s in segments if s['name'] == '__PRELINK_INFO')
    metadata = np.unserialize_xml(read(info['va'], info['filesize']))
    entry = next(e for e in metadata['_PrelinkInfoDictionary'] if e['CFBundleIdentifier'] == FAMILY)
    kext = np.macho(data, np.backed_offset(segments, entry['_PrelinkExecutableLoadAddr'], 32, len(data)))
    seg = {s['name']: s for s in kext['segments']}
    text, const = seg['__TEXT_EXEC'], seg['__DATA_CONST']
    require(hashlib.sha256(read(text['va'], text['size'])).hexdigest() == FAMILY_TEXT_SHA256, 'IONetworkingFamily instructions differ')
    names = {}
    for address, symbols in np.exports(data, top).items():
        for name in symbols:
            names[name] = address

    def stub_target(va):
        first, second, third = struct.unpack('<3I', read(va, 12))
        require(is_adrp(first, 16) and second & 0xffc003ff == 0xf9400210 and third == 0xd61f0200, 'import stub shape')
        return qword(page(va, first) + ((second >> 10) & 4095) * 8)

    def calls(va, name):
        return stub_target(va) == names[name]

    cstrings = next(s for s in seg['__TEXT']['sections'] if s['name'] == '__cstring')
    strings = read(cstrings['va'], cstrings['size'])
    code = read(text['va'], text['size'])
    words = struct.unpack(f'<{len(code) // 4}I', code)
    base_tables = {b: struct.unpack(f'<{n}Q', read(names[f'__ZTV{len(b)}{b}'] + 16, n * 8)) for b, n in BASES.items()}
    report, bindings = {'kernelcache_sha256': ORIGINAL_SHA256, 'family': FAMILY, 'family_text_sha256': FAMILY_TEXT_SHA256,
                        'slide': slide, 'classes': {}}, {}
    for cls, (base_name, size, slots, pure, overrides, bound) in CLASSES.items():
        base, base_slots = base_tables[base_name], BASES[base_name]
        at = strings.find(cls.encode() + b'\0')
        while at > 0 and strings[at - 1] != 0:
            at = strings.find(cls.encode() + b'\0', at + 1)
        require(at >= 0, cls + ' name')
        name_va = cstrings['va'] + at
        # OSMetaClass(this = metaclass, "name", &IOService::gMetaClass, size): adrp/add x0, adrp/add x1, adrp/ldr x2, mov w3, bl.
        found = []
        for i in range(2, len(words) - 6):
            pc = text['va'] + 4 * i
            if not (is_adrp(words[i], 1) and is_add(words[i + 1], 1) and page(pc, words[i]) + ((words[i + 1] >> 10) & 4095) == name_va):
                continue
            # The static initializer loads the metaclass object into x0; the two MetaClass constructors receive it from their caller.
            meta = page(pc - 8, words[i - 2]) + ((words[i - 1] >> 10) & 4095) if is_adrp(words[i - 2], 0) and is_add(words[i - 1], 0) else None
            require(is_adrp(words[i + 2], 2) and words[i + 3] & 0xffc003ff == 0xf9400042, cls + ' superclass load')
            superclass = qword(page(pc + 8, words[i + 2]) + ((words[i + 3] >> 10) & 4095) * 8)
            require(words[i + 4] & 0xffe0001f == 0x52800003, cls + ' size load')
            require(is_bl(words[i + 5]) and calls(branch(pc + 20, words[i + 5]), '__ZN11OSMetaClassC2EPKcPKS_j'), cls + ' metaclass constructor')
            found.append((meta, superclass, (words[i + 4] >> 5) & 0xffff))
        require(len({f[1:] for f in found}) == 1 and len([f for f in found if f[0]]) == 1, cls + ' unique metaclass registration')
        meta, superclass, registered = next(f for f in found if f[0])
        require(superclass == names[f'__ZN{len(base_name)}{base_name}10gMetaClassE'], cls + ' derives from ' + base_name)
        require(registered == size, cls + ' instance size')
        # The dispatch table: two zero words, then the methods; getMetaClass (slot 7) answers this metaclass.
        blob = read(const['va'], const['size'])
        tables = []
        for off in range(16, len(blob) - 64, 8):
            if blob[off - 16:off] != bytes(16):
                continue
            getter = struct.unpack_from('<Q', blob, off + 56)[0]
            if not text['va'] <= getter < text['va'] + text['size']:
                continue
            a, add, ret = struct.unpack('<3I', read(getter, 12))
            if is_adrp(a, 0) and is_add(add, 0) and ret == 0xd65f03c0 and page(getter, a) + ((add >> 10) & 4095) == meta:
                tables.append(const['va'] + off)
        require(len(tables) == 1, cls + ' unique dispatch table')
        vptr = tables[0]
        methods = struct.unpack(f'<{slots}Q', read(vptr, slots * 8))
        after = qword(vptr + slots * 8)
        require(all(methods) and not (text['va'] <= after < text['va'] + text['size'] or after in names.values()), cls + ' slot count')
        require([i for i, m in enumerate(methods) if m == names['___cxa_pure_virtual']] == pure, cls + ' pure slots')
        require([i for i in range(base_slots) if methods[i] != base[i]] == overrides, cls + ' ' + base_name + ' overrides')
        report['classes'][cls] = {'base': base_name, 'size': size, 'slots': slots, 'pure': pure, 'overrides': overrides, 'bound': bound}
        if not bound:
            require(slots == base_slots, cls + ' adds no slots of its own')
            continue
        for i in range(IOSERVICE_SLOTS, slots):
            require(methods[i] == names['___cxa_pure_virtual'] or text['va'] <= methods[i] < text['va'] + text['size'], cls + ' own slot in family')
        # Protected constructor: stp, mov, bl IOService(const OSMetaClass *), adrp/add x8 = vptr, str x8, [x0], ldp, ret.
        constructors = []
        for i in range(len(words) - 8):
            pc = text['va'] + 4 * i
            w = words[i:i + 8]
            if (w[0], w[1]) == (0xa9bf7bfd, 0x910003fd) and is_bl(w[2]) and is_adrp(w[3], 8) and is_add(w[4], 8) and \
                    w[5:] == (0xf9000008, 0xa8c17bfd, 0xd65f03c0) and page(pc + 12, w[3]) + ((w[4] >> 10) & 4095) == vptr:
                require(calls(branch(pc + 8, w[2]), '__ZN9IOServiceC2EPK11OSMetaClass'), cls + ' constructor calls IOService')
                constructors.append(pc)
        # A class whose complete-object constructor the compiler did not alias emits C1 and C2 as two identical copies.
        shapes = {(read(c, 8), branch(c + 8, word(c + 8)), read(c + 12, 20)) for c in constructors}
        require(len(constructors) in (1, 2) and len(shapes) == 1, cls + ' protected constructor')
        destructor = methods[0] - 4
        d = word(destructor)
        require(is_b(d) and calls(branch(destructor, d), '__ZN9IOServiceD2Ev'), cls + ' base destructor is IOService')
        bindings[f'__ZN{len(cls)}{cls}C2EPK11OSMetaClass'] = constructors[0] + slide
        bindings[f'__ZN{len(cls)}{cls}D2Ev'] = destructor + slide
        bindings[f'__ZN{len(cls)}{cls}10gMetaClassE'] = meta + slide
        bindings[f'__ZTV{len(cls)}{cls}'] = vptr - 16 + slide
        for i in range(IOSERVICE_SLOTS, slots):
            if i not in pure:
                bindings[mangled(cls, f'original{i}')] = methods[i] + slide
    return report, bindings


def header(report):
    lines = ['// SPDX-License-Identifier: BSD-2-Clause-Patent',
             '// Generated by tools/record_network_bindings.py from the pinned original kernelcache: the original IONetworkingFamily',
             '// classes the Wi-Fi registry derives from, as opaque storage and slots. Their constructor, destructor, metaclass and own',
             '// slots are bound to the original code (network-original-bindings.S); a subclass must replace every IOService slot the',
             '// original overrides, so no original method runs on an object the original family did not initialize.',
             '#pragma once', '#include <IOKit/IOService.h>', '']
    for cls, c in report['classes'].items():
        if not c['bound']:
            owned = [i for i in c['overrides'] if i not in (0, 1, 7)]
            lines += [f'// {cls} is allocated by name; these {c["base"]} slots of its table are replaced at run time.',
                      f'enum {{ k{cls}Slots = {c["slots"]} }};',
                      f'static const unsigned short k{cls}Replaced[] = {{{", ".join(map(str, owned))}}};', '']
            continue
        lines += [f'class {cls} : public IOService {{', f' OSDeclareAbstractStructors({cls})',
                  f' unsigned char originalPrivate[{c["size"]:#x} - sizeof(IOService)];', 'public:']
        for i in range(IOSERVICE_SLOTS, c['slots']):
            if i in c['pure']:
                lines.append(f' virtual void original{i}() = 0;')
            else:
                lines.append(f' virtual __attribute__((deprecated("Unqualified prototype: never call from source"))) void original{i}();')
        lines += ['};', f'static_assert(sizeof({cls}) == {c["size"]:#x}, "Original {cls} instance extent");',
                  f'enum {{ k{cls}OriginalOverrides = {len(c["overrides"])} }};', '']
    return '\n'.join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('kernelcache', type=Path)
    parser.add_argument('--slide', type=lambda v: int(v, 0), default=-0x1000000)
    parser.add_argument('--output', type=Path, default=OUTPUT)
    args = parser.parse_args()
    report, bindings = record(args.kernelcache, args.slide)
    args.output.mkdir(parents=True, exist_ok=True)
    (args.output / 'OriginalIONetworkingABI.h').write_text(header(report))
    (args.output / 'network-original-bindings.S').write_text(
        '// IONetworkingFamily in the original kernelcache at the OS profile slide: tools/record_network_bindings.py\n' +
        ''.join(f'.globl {n}\n.set {n}, {a & 0xffffffffffffffff:#x}\n' for n, a in sorted(bindings.items())))
    (args.output / 'network-original.json').write_text(json.dumps(report, indent=2) + '\n')
    print('recorded', len(bindings), 'original IONetworkingFamily bindings for', ', '.join(report['classes']))


if __name__ == '__main__':
    main()
