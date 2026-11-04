#!/usr/bin/env python3
"""Build the Apple USB host stack's platform side, CoachzUsbHost.cpp, for the kernel (coachz-usb-host=1)."""
# SPDX-License-Identifier: BSD-2-Clause-Patent
# The controller is an original AppleNubT8011USBXHCI whose table takes AppleUSBXHCI's slots by address (usb-original-bindings.S, written
# by tools/record_usb_bindings.py). The SMMU kit is referenced, not compiled: libCoachzGpu.a already carries its objects, and every
# archive is force-loaded into the same kernel.
from pathlib import Path
import re
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'kits/XnuPrelink'))
from native_prelink import macho

KIT = ROOT / 'kits/coachz'
SMMU = ROOT / 'kits/ArmSmmu4K'
OUT = ROOT / 'build/coachz-usb-host'
OUT.mkdir(parents=True, exist_ok=True)
KIT_CALLS = {'_ArmSmmu4KLivePrepare', '_ArmSmmu4KProgramDisabledContext', '_ArmSmmu4KLiveUpdateContextBatch'}


def require(value, why):
    if not value:
        raise SystemExit('build-coachz-usb-host: ' + why)


def run(command, **kwargs):
    return subprocess.run([str(part) for part in command], check=True, **kwargs)


record = __import__('json').loads((ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json').read_text().strip().rstrip(','))
arguments, command, index = record['arguments'], [], 0
while index < len(arguments):
    value = arguments[index]
    if value == record['file']:
        command.append(str(KIT / 'CoachzUsbHost.cpp'))
    elif value in ('-o', '-MF', '-MT', '-MJ'):
        if value == '-o':
            command += ['-o', str(OUT / 'CoachzUsbHost.o')]
        index += 1
    elif value == '-Xclang' and arguments[index + 1].startswith('-W'):
        index += 1
    elif not value.startswith('-W'):
        command.append(value)
    index += 1
command += ['-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter', '-Wno-unnecessary-virtual-specifier', '-Wno-c99-designator',
            '-Wno-reorder-init-list', '-Wno-deprecated-enum-enum-conversion', '-Wno-missing-field-initializers', '-I', str(SMMU)]
run(command, cwd=record['directory'])

raw = (OUT / 'CoachzUsbHost.o').read_bytes()
image = macho(raw)
symtab = next(offset for c, offset, _ in image['commands'] if c == 2)
sym_off, count, str_off, _ = struct.unpack_from('<4I', raw, symtab + 8)
symbols = []
for i in range(count):
    name_at, kind, _, _, value = struct.unpack_from('<IBBHQ', raw, sym_off + i * 16)
    symbols.append((raw[str_off + name_at:raw.index(b'\0', str_off + name_at)].decode(), kind))
undefined = {name for name, kind in symbols if kind == 1}
defined = {name for name, kind in symbols if kind & 0xe == 0xe}
require('_coachz_original_xhci_table' in undefined, 'the controller table is not taken from the recorded AppleUSBXHCI table')
require(KIT_CALLS <= undefined, 'the SMMU kit calls: ' + ', '.join(sorted(KIT_CALLS - undefined)))
require(not any(n.startswith('_ArmSmmu4K') for n in defined), 'the SMMU kit must come from libCoachzGpu.a only')
require({'_coachz_usb_host_start', '_coachz_usb_host_enabled'} <= defined, 'entry points')
require(not any(re.search(r'AppleUSBXHCI|AppleNubT8011', n) for n in undefined), 'no original USB class is linked by name')

sdk = subprocess.check_output(['xcrun', '--sdk', 'iphoneos', '--show-sdk-path'], text=True).strip()
bindings = OUT / 'usb-original-bindings.o'
run(['xcrun', 'clang', '-target', 'arm64-apple-ios17.0', '-isysroot', sdk, '-c', KIT / 'usb-original-bindings.S', '-o', bindings])
archive = OUT / 'libCoachzUsbHost.a'
archive.unlink(missing_ok=True)
run(['xcrun', 'ar', 'rcs', archive, OUT / 'CoachzUsbHost.o', bindings])
print('Coachz: USB host platform side built (apps SMMU mapper, nub, controller from AppleNubT8011USBXHCI); hardware not accepted')
