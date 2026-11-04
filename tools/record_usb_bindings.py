#!/usr/bin/env python3
"""Record the original Apple USB host controller classes the USB host route builds its controller from.

The controller is an AppleNubT8011USBXHCI instance (AppleUSBXHCI is abstract) whose dispatch table is a copy with every slot the T8011
subclass overrides taken back from AppleUSBXHCI's own table, and start replaced (CoachzUsbHost.cpp). This finds, in the pinned original
kernelcache, both classes' metaclass registrations and their one dispatch table each by instruction shape, checks the instance sizes,
the slot counts, the subclass's overrides, that its start masks the controller's buffer cache options at +0x494, and that the IOService
slots AppleUSBXHCI::start calls on its provider are getDeviceMemoryWithIndex and getInterruptType. It writes the slot numbers and the
address of AppleUSBXHCI's table at the OS profile slide. The written files hold addresses, sizes and slot numbers only.

Usage: record_usb_bindings.py <original kernelcache.decompressed> [--slide -0x1000000] [--output DIR]
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'upstream/unixnu/kits/XnuPrelink'))
sys.path.insert(0, str(ROOT / 'tools'))
import native_prelink as np  # noqa: E402
from record_network_bindings import ORIGINAL_SHA256, page, branch, is_adrp, is_add, is_bl, require as _require  # noqa: E402

HOST_FAMILY = 'com.apple.iokit.IOUSBHostFamily'
HID_FAMILY = 'com.apple.iokit.IOHIDFamily'
XHCI = 'com.apple.driver.usb.AppleUSBXHCI'
T8011 = 'com.apple.driver.usb.AppleUSBHostT8011'
# class: (kext, superclass, superclass kext, instance size, slots)
CLASSES = {
    'AppleUSBHostController': (HOST_FAMILY, 'IOService', None, None, None),
    'AppleUSBXHCI': (XHCI, 'AppleUSBHostController', HOST_FAMILY, 0xe48, 299),
    'AppleNubT8011USBXHCI': (T8011, 'AppleUSBXHCI', XHCI, 0xe68, 301),
    'IOHIDInterface': (HID_FAMILY, 'IOService', None, None, None),
}
# The base's keyboard and touchpad bridge (CoachzUsbHost.cpp) opens the original IOHIDInterface with a report action: the first slot
# past IOService's is open(client, options, action, refCon), which keeps the action at +0x98 and the refCon and client at +0xa0;
# handleReport (181) calls that action.
HID_OPEN, HID_REPORT, HID_ACTION = 168, 181, 0x98
# The slots AppleNubT8011USBXHCI overrides in AppleUSBXHCI's table (docs/plans/usb-host.md): destructors, getMetaClass, free, start,
# message, the deactivation thread call, lower/raiseOnePowerStateTo, two small hooks, createPort and the register accessors 289-297;
# 299 and 300 are its own. The accessors use only the register window they are given, and the subclass's 64-bit write (293) is two
# 32-bit writes, low word first, where AppleUSBXHCI's is one 64-bit access: Linux's xHCI driver writes 64-bit registers so
# (lo_hi_writeq). The controller keeps the subclass's accessors and takes the others back from AppleUSBXHCI.
NUB_OVERRIDES = [0, 1, 7, 14, 86, 132, 177, 180, 182, 183, 193, 240, 273, 289, 290, 291, 292, 293, 294, 295, 296, 297]
KEPT = [289, 290, 291, 292, 293, 294, 295, 296, 297]
WRITE64 = 293
START = 86
CACHE_OPTIONS = 0x494
# AppleUSBXHCI::start calls these on its provider (vtable +0x3c8 with index 0, and +0x3f8 per interrupt index).
PROVIDER_SLOTS = {121: '__ZN9IOService24getDeviceMemoryWithIndexEj', 127: '__ZN9IOService16getInterruptTypeEiPi'}
OUTPUT = ROOT / 'platform/coachz-prelink/package'


def require(value, message):
    if not value:
        raise SystemExit('record_usb_bindings: ' + message)


def record(kernelcache, slide):
    data = kernelcache.read_bytes()
    require(hashlib.sha256(data).hexdigest() == ORIGINAL_SHA256, 'unexpected original kernelcache')
    top = np.macho(data)
    segments = top['segments']

    def read(va, length):
        at = np.backed_offset(segments, va, length, len(data))
        return data[at:at + length]

    def qword(va):
        return struct.unpack('<Q', read(va, 8))[0]

    info = next(s for s in segments if s['name'] == '__PRELINK_INFO')
    metadata = np.unserialize_xml(read(info['va'], info['filesize']))
    names = {}
    for address, symbols in np.exports(data, top).items():
        for name in symbols:
            names[name] = address

    kexts = {}
    for bundle in {c[0] for c in CLASSES.values()}:
        entry = next(e for e in metadata['_PrelinkInfoDictionary'] if e['CFBundleIdentifier'] == bundle)
        kext = np.macho(data, np.backed_offset(segments, entry['_PrelinkExecutableLoadAddr'], 32, len(data)))
        seg = {s['name']: s for s in kext['segments']}
        text = seg['__TEXT_EXEC']
        code = read(text['va'], text['size'])
        cstrings = next(s for s in seg['__TEXT']['sections'] if s['name'] == '__cstring')
        kexts[bundle] = {'text': text, 'const': seg['__DATA_CONST'], 'words': struct.unpack(f'<{len(code) // 4}I', code),
                         'cstrings': cstrings, 'strings': read(cstrings['va'], cstrings['size']),
                         'text_sha256': hashlib.sha256(code).hexdigest()}

    def in_text(bundle, va):
        text = kexts[bundle]['text']
        return text['va'] <= va < text['va'] + text['size']

    def stub_target(va):
        first, second, third = struct.unpack('<3I', read(va, 12))
        require(is_adrp(first, 16) and second & 0xffc003ff == 0xf9400210 and third == 0xd61f0200, 'import stub shape')
        return qword(page(va, first) + ((second >> 10) & 4095) * 8)

    def metaclass(cls, bundle):
        k = kexts[bundle]
        strings, words, text = k['strings'], k['words'], k['text']
        at = strings.find(cls.encode() + b'\0')
        while at > 0 and strings[at - 1] != 0:
            at = strings.find(cls.encode() + b'\0', at + 1)
        require(at >= 0, cls + ' name')
        name_va = k['cstrings']['va'] + at
        # OSMetaClass(this, "name", &Super::gMetaClass, size): adrp/add x1 = name, adrp/ldr x2 = superclass, mov w3, bl.
        found = []
        for i in range(2, len(words) - 6):
            pc = text['va'] + 4 * i
            if not (is_adrp(words[i], 1) and is_add(words[i + 1], 1) and page(pc, words[i]) + ((words[i + 1] >> 10) & 4095) == name_va):
                continue
            if not (is_adrp(words[i + 2], 2) and words[i + 3] & 0xffc003ff == 0xf9400042):
                continue
            meta = page(pc - 8, words[i - 2]) + ((words[i - 1] >> 10) & 4095) if is_adrp(words[i - 2], 0) and is_add(words[i - 1], 0) else None
            superclass = qword(page(pc + 8, words[i + 2]) + ((words[i + 3] >> 10) & 4095) * 8)
            if words[i + 4] & 0xffe0001f != 0x52800003 or not is_bl(words[i + 5]):
                continue
            if stub_target(branch(pc + 20, words[i + 5])) != names['__ZN11OSMetaClassC2EPKcPKS_j']:
                continue
            found.append((meta, superclass, (words[i + 4] >> 5) & 0xffff))
        require(len({f[1:] for f in found}) == 1 and len([f for f in found if f[0]]) == 1, cls + ' unique metaclass registration')
        return next(f for f in found if f[0])

    def table(cls, bundle, meta, slots, check=True):
        k = kexts[bundle]
        const, text = k['const'], k['text']
        blob = read(const['va'], const['size'])
        tables = []
        for off in range(16, len(blob) - 64, 8):
            if blob[off - 16:off] != bytes(16):
                continue
            getter = struct.unpack_from('<Q', blob, off + 56)[0]
            if not in_text(bundle, getter):
                continue
            a, add, ret = struct.unpack('<3I', read(getter, 12))
            if is_adrp(a, 0) and is_add(add, 0) and ret == 0xd65f03c0 and page(getter, a) + ((add >> 10) & 4095) == meta:
                tables.append(const['va'] + off)
        require(len(tables) == 1, cls + ' unique dispatch table')
        vptr = tables[0]
        methods = struct.unpack(f'<{slots}Q', read(vptr, slots * 8))
        after = qword(vptr + slots * 8)
        require(all(methods), cls + ' slots')
        require(not check or (not any(in_text(b, after) for b in kexts) and after not in names.values()), cls + ' slot count')
        return vptr, methods

    report = {'kernelcache_sha256': ORIGINAL_SHA256, 'slide': slide, 'text_sha256': {b: kexts[b]['text_sha256'] for b in sorted(kexts)},
              'classes': {}}
    metas, tables = {}, {}
    for cls, (bundle, superclass, super_bundle, size, slots) in CLASSES.items():
        meta, parent, registered = metaclass(cls, bundle)
        metas[cls] = meta
        if super_bundle is None:
            require(parent == names[f'__ZN{len(superclass)}{superclass}10gMetaClassE'], cls + ' derives from ' + superclass)
            if cls == 'IOHIDInterface':
                tables[cls] = table(cls, bundle, meta, HID_REPORT + 1, check=False)
            continue
        require(parent == metas[superclass], cls + ' derives from ' + superclass)
        require(registered == size, cls + ' instance size')
        tables[cls] = table(cls, bundle, meta, slots)
        report['classes'][cls] = {'kext': bundle, 'superclass': superclass, 'size': size, 'slots': slots}
    xhci_vptr, xhci = tables['AppleUSBXHCI']
    nub_vptr, nub = tables['AppleNubT8011USBXHCI']
    overrides = [i for i in range(len(xhci)) if nub[i] != xhci[i]]
    require(overrides == NUB_OVERRIDES, 'AppleNubT8011USBXHCI overrides ' + ', '.join(
        f'{i} ({xhci[i]:#x} -> {nub[i]:#x})' for i in overrides) + f'; pure virtual {names["___cxa_pure_virtual"]:#x}')
    require(all(in_text(T8011, nub[i]) for i in overrides + [299, 300]), 'the subclass overrides are its own code')

    def write_calls(va, words):
        """The imports a short accessor reaches through bl, up to and including its tail b."""
        targets = []
        for i, w in enumerate(struct.unpack(f'<{words}I', read(va, 4 * words))):
            if w & 0x7c000000 == 0x14000000:
                targets.append(stub_target(branch(va + 4 * i, w)))
            if w & 0xfc000000 == 0x14000000 or w == 0xd65f03c0:
                break
        return targets
    split = write_calls(nub[WRITE64], 16)
    require(split == [names['_ml_io_write32']] * 2, 'the subclass writes 64-bit registers as two 32-bit writes')
    require(write_calls(xhci[WRITE64], 4) == [names['_ml_io_write64']], 'AppleUSBXHCI writes 64-bit registers in one access')
    require(names['___cxa_pure_virtual'] not in xhci[2:], 'AppleUSBXHCI has no pure slot past the destructors')
    # The subclass's start clears the cache mode bits of the buffer options: ldr wN, [xM, #0x494] ... str wN, [xM, #0x494].
    start = struct.unpack('<512I', read(nub[START], 2048))
    loads = [i for i, w in enumerate(start) if w & 0xffc00000 == 0xb9400000 and (w >> 10) & 0xfff == CACHE_OPTIONS // 4]
    stores = [i for i, w in enumerate(start) if w & 0xffc00000 == 0xb9000000 and (w >> 10) & 0xfff == CACHE_OPTIONS // 4]
    require(any(s - l == 2 and start[l + 1] & 0x7f800000 == 0x12000000 for l in loads for s in stores), 'start masks +0x494')
    _, hid = tables['IOHIDInterface']
    service = struct.unpack('<168Q', read(names['__ZTV9IOService'] + 16, 168 * 8))
    require(hid[HID_OPEN] != service[HID_OPEN - 1] and in_text(HID_FAMILY, hid[HID_OPEN]), 'IOHIDInterface slot 168 is its own')
    opened = struct.unpack('<40I', read(hid[HID_OPEN], 160))
    # str xA, [xT, #0x98] and stp xB, xC, [xT, #0xa0]: the action, then refCon and client.
    require(any(w & 0xffc00000 == 0xf9000000 and (w >> 10) & 0xfff == HID_ACTION // 8 for w in opened), 'open keeps the action at +0x98')
    require(any(w & 0xffc00000 == 0xa9000000 and (w >> 15) & 0x7f == (HID_ACTION + 8) // 8 for w in opened), 'open keeps refCon, client')
    first = struct.unpack('<I', read(hid[HID_REPORT], 4))[0]
    require(first & 0xffc003e0 == 0xf9400000 and (first >> 10) & 0xfff == HID_ACTION // 8, 'handleReport calls the kept action')
    for slot, name in PROVIDER_SLOTS.items():
        require(service[slot] == names[name], f'IOService slot {slot} is {name}')
    report['nub_overrides'] = NUB_OVERRIDES
    report['kept'] = KEPT
    report['start_slot'] = START
    report['cache_options_offset'] = CACHE_OPTIONS
    report['provider_slots'] = {str(k): v for k, v in PROVIDER_SLOTS.items()}
    report['hid_interface_open_slot'] = HID_OPEN
    return report, {'_coachz_original_xhci_table': xhci_vptr + slide}


def header(report):
    xhci, nub = report['classes']['AppleUSBXHCI'], report['classes']['AppleNubT8011USBXHCI']
    replaced = [i for i in report['nub_overrides'] if i not in (0, 1, 7) and i not in report['kept']]
    return '\n'.join([
        '// SPDX-License-Identifier: BSD-2-Clause-Patent',
        '// Generated by tools/record_usb_bindings.py from the pinned original kernelcache: the controller is an AppleNubT8011USBXHCI',
        '// instance whose table is a copy with these subclass slots taken from AppleUSBXHCI\'s table (coachz_original_xhci_table in',
        '// usb-original-bindings.S, its slot 0); its destructors, getMetaClass and register accessors (64-bit writes as two 32-bit',
        '// writes, low word first) stay the subclass\'s.',
        '#pragma once', '',
        f'enum {{ kAppleUSBXHCISlots = {xhci["slots"]}, kAppleNubT8011USBXHCISlots = {nub["slots"]}, kAppleNubT8011USBXHCISize = {nub["size"]:#x} }};',
        f'enum {{ kAppleUSBXHCIStartSlot = {report["start_slot"]}, kAppleUSBHostControllerCacheOptions = {report["cache_options_offset"]:#x} }};',
        f'static const unsigned short kAppleNubT8011USBXHCIReplaced[] = {{{", ".join(map(str, replaced))}}};',
        '// IOHIDInterface::open(IOService *client, IOOptionBits, InterruptReportAction, void *refCon) in the original IOHIDFamily.',
        f'enum {{ kIOHIDInterfaceOpenSlot = {report["hid_interface_open_slot"]} }};',
        'extern "C" void *coachz_original_xhci_table[];', ''])


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('kernelcache', type=Path)
    parser.add_argument('--slide', type=lambda v: int(v, 0), default=-0x1000000)
    parser.add_argument('--output', type=Path, default=OUTPUT)
    args = parser.parse_args()
    report, bindings = record(args.kernelcache, args.slide)
    args.output.mkdir(parents=True, exist_ok=True)
    (args.output / 'OriginalAppleUSBABI.h').write_text(header(report))
    (args.output / 'usb-original-bindings.S').write_text(
        '// AppleUSBXHCI in the original kernelcache at the OS profile slide: tools/record_usb_bindings.py\n' +
        ''.join(f'.globl {n}\n.set {n}, {a & 0xffffffffffffffff:#x}\n' for n, a in sorted(bindings.items())))
    (args.output / 'usb-original.json').write_text(json.dumps(report, indent=2) + '\n')
    print('recorded the original USB host controller classes:', ', '.join(report['classes']))


if __name__ == '__main__':
    main()
