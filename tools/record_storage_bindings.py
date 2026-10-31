#!/usr/bin/env python3
"""Record the original IOBlockStorageDevice addresses that a block driver links against.

The static part of UniXNU scripts/audit-storage-interface.py (BSD-2-Clause-Patent, Copyright (c)
2026, UniXNU contributors): it compiles
the public class to learn the dispatch shape, finds the one matching table in the original
IOStorageFamily of a prelinked kernel, and checks the constructor, destructor and metaclass getter
by their instruction shape. The addresses depend only on the pinned kernelcache and slide, so they
are recorded like UniXNU's AES bindings. UniXNU's live QEMU check of the class sizes is not repeated.

Usage: record_storage_bindings.py <build tree> <kernel.macho> <driver object> <output .S>
"""
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys

TREE, KERNEL, DRIVER, OUTPUT = map(Path, sys.argv[1:5])
sys.path.insert(0, str(TREE / 'kits/XnuPrelink'))
import native_prelink as np


def require(value, message):
    if not value:
        raise SystemExit(message)


def symbol_table(image, data):
    at = next(offset for command, offset, _ in image['commands'] if command == 2)
    symoff, count, stroff, _ = struct.unpack_from('<4I', data, at + 8)
    table = []
    for i in range(count):
        index, kind, _, _, value = struct.unpack_from('<IBBHQ', data, symoff + i * 16)
        table.append(dict(name=data[stroff + index:data.index(b'\0', stroff + index)].decode(), value=value, kind=kind))
    return table


source = TREE / 'refer/IOStorageFamily'
out = TREE / 'build/storage-abi'
headers = out / 'include/IOKit/storage'
headers.mkdir(parents=True, exist_ok=True)
for path in source.glob('*.h'):
    shutil.copyfile(path, headers / path.name)
sdk = Path(subprocess.check_output(['xcrun', '--sdk', 'macosx', '--show-sdk-path'], text=True).strip())
shutil.copyfile(sdk / 'usr/include/TargetConditionals.h', out / 'include/TargetConditionals.h')
record = json.loads((TREE / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json').read_text().strip().rstrip(','))
reference = out / 'IOBlockStorageDevice-reference.o'
command, index, arguments = [], 0, record['arguments']
while index < len(arguments):
    value = arguments[index]
    if value == record['file']:
        command.append(str(source / 'IOBlockStorageDevice.cpp'))
    elif value == '-o':
        command += ['-o', str(reference)]
        index += 1
    elif value in ('-MF', '-MJ'):
        index += 1
    elif value == '-Xclang' and arguments[index + 1].startswith('-W'):
        index += 1
    elif not value.startswith('-W') and value not in ('-MD', '-MP'):
        command.append(value)
    index += 1
command += ['-I', str(out / 'include'), '-I', str(TREE / '.cache/xnu-work/osfmk'), '-w']
result = subprocess.run(command, cwd=record['directory'], capture_output=True, text=True)
require(not result.returncode, (result.stdout + result.stderr)[-3000:])

obj = reference.read_bytes()
image = np.macho(obj)
symbols = symbol_table(image, obj)
vtable = next(s['value'] for s in symbols if s['name'] == '__ZTV20IOBlockStorageDevice')
end = min(s['value'] for s in symbols if not s['kind'] & 0xe0 and s['kind'] & 0xe == 0xe and s['value'] > vtable)
relocations = {}
for segment in image['segments']:
    for section in segment['sections']:
        if section['va'] <= vtable < section['va'] + section['size']:
            ro, rc = struct.unpack_from('<II', obj, section['command_offset'] + 56)
            for i in range(rc):
                address, word = struct.unpack_from('<II', obj, ro + i * 8)
                if word >> 27 & 1 and word >> 28 == 0 and word >> 25 & 3 == 3:
                    relocations[section['va'] + address] = symbols[word & 0xffffff]['name']
methods = []
for at in range(vtable + 16, end, 8):
    if at not in relocations:
        break
    methods.append(relocations[at])
require(len(methods) == 190 and methods.count('___cxa_pure_virtual') == 14, 'Public iOS class dispatch shape changed')

kernel = KERNEL.read_bytes()
plan = json.loads(KERNEL.with_suffix('.json').read_text())
require(plan['original_sha256'] == 'f18b4df865405f2a287b84977d7aca745e056d656d9fc9df1662970ce2969de7', 'Unexpected original kernelcache')
native = np.macho(kernel)
exports = np.exports(kernel, native)
by_name = {name: va for va, names in exports.items() for name in names}
extents = [e for e in plan['extents'] if e['module'] == 'com.apple.iokit.IOStorageFamily']
const = next(e for e in extents if e['segment'] == '__DATA_CONST')
text = next(e for e in extents if e['segment'] == '__TEXT_EXEC')


def read(va, length):
    at = np.backed_offset(native['segments'], va, length, len(kernel))
    return kernel[at:at + length]


blob = read(const['va'], const['size'])
found = []
for at in range(0, len(blob) - (len(methods) + 2) * 8 + 1, 8):
    if blob[at:at + 16] != bytes(16):
        continue
    pointers = struct.unpack_from('<' + 'Q' * len(methods), blob, at + 16)
    if all(pointers[i] == by_name[name] for i, name in enumerate(methods) if name in by_name):
        found.append((const['va'] + at, pointers))
require(len(found) == 1, 'Cannot uniquely identify the original block-storage class dispatch')
table, pointers = found[0]
require(sum(name in by_name for name in methods) == 176, 'Unexpected original/native kernel binding count')
for i, name in enumerate(methods):
    if name not in by_name:
        require(text['va'] <= pointers[i] < text['va'] + text['size'], 'Owned storage method outside original text')


def relative_branch(address, instruction):
    immediate = instruction & 0x3ffffff
    if immediate & 0x2000000:
        immediate -= 0x4000000
    return address + immediate * 4


def page_immediate(word):
    immediate = ((word >> 5 & 0x7ffff) << 2) | (word >> 29 & 3)
    return immediate - 0x200000 if immediate & 0x100000 else immediate


def thunk_names(address):
    a, load, branch = struct.unpack('<3I', read(address, 12))
    require(a & 0x9f00001f == 0x90000010 and load & 0xffc003ff == 0xf9400210 and branch == 0xd61f0200, 'Unqualified import thunk')
    slot = (address & ~4095) + page_immediate(a) * 4096 + (load >> 10 & 4095) * 8
    return exports.get(struct.unpack('<Q', read(slot, 8))[0], [])


constructors = []
for address in range(text['va'], text['va'] + text['size'] - 32, 4):
    words = struct.unpack('<8I', read(address, 32))
    if (words[:2] != (0xa9bf7bfd, 0x910003fd) or words[2] & 0xfc000000 != 0x94000000 or
            words[3] & 0x9f00001f != 0x90000008 or words[4] & 0xffc003ff != 0x91000108 or
            words[5:] != (0xf9000008, 0xa8c17bfd, 0xd65f03c0)):
        continue
    stored = ((address + 12) & ~4095) + page_immediate(words[3]) * 4096 + (words[4] >> 10 & 4095)
    if stored == table + 16:
        constructors.append((address, relative_branch(address + 8, words[2])))
require(len(constructors) == 1 and '__ZN9IOServiceC2EPK11OSMetaClass' in thunk_names(constructors[0][1]), 'Unqualified protected constructor')
destructor = pointers[0] - 4
word = struct.unpack('<I', read(destructor, 4))[0]
require(word & 0xfc000000 == 0x14000000 and '__ZN9IOServiceD2Ev' in thunk_names(relative_branch(destructor, word)), 'Unqualified base destructor')
require(all(read(pointers[i], 4) == struct.pack('<I', 0xd4200020) for i in (0, 1)), 'Unexpected abstract destructors')
a, add, ret = struct.unpack('<3I', read(pointers[7], 12))
require(a & 0x9f00001f == 0x90000000 and add & 0xffc003ff == 0x91000000 and ret == 0xd65f03c0, 'Unqualified metaclass getter')
meta = (pointers[7] & ~4095) + page_immediate(a) * 4096 + (add >> 10 & 4095)

possible = {name: pointers[i] for i, name in enumerate(methods) if name not in by_name and name != '___cxa_pure_virtual'}
possible.update({'__ZN20IOBlockStorageDeviceC2EPK11OSMetaClass': constructors[0][0], '__ZN20IOBlockStorageDeviceD2Ev': destructor,
                 '__ZN20IOBlockStorageDevice10gMetaClassE': meta, '__ZTV20IOBlockStorageDevice': table})
driver = DRIVER.read_bytes()
undefined = [s['name'] for s in symbol_table(np.macho(driver), driver) if s['kind'] == 1 and 'IOBlockStorageDevice' in s['name']]
unknown = [name for name in undefined if name not in possible]
require(not unknown, 'Unqualified original storage imports: ' + ', '.join(unknown))
require(not any(re.search(r'IOBlockStorageDeviceD[01]Ev$', name) for name in undefined), 'Concrete driver must not use abstract destructors')
OUTPUT.write_text('// IOBlockStorageDevice in the original kernelcache at the OS profile slide: tools/record_storage_bindings.py\n' +
                  ''.join(f'.globl {name}\n.set {name}, {possible[name]:#x}\n' for name in sorted(undefined)))
print('recorded', len(undefined), 'original storage imports; slide', plan['slide'] if 'slide' in plan else '-')
