#!/usr/bin/env python3
"""Build the Wi-Fi network side, the Ethernet interface and Apple80211 requests of CoachzWifiNet.cpp, for the kernel."""
# SPDX-License-Identifier: BSD-2-Clause-Patent
# The registry classes derive from the original IONetworkingFamily (OriginalIONetworkingABI.h and network-original-bindings.S, written by
# tools/record_network_bindings.py). The compiled dispatch tables are checked here: every IOService slot the original class overrides
# must be this file's own, and the original class's own slots must reach the recorded original methods.
import json
from pathlib import Path
import re
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(ROOT / 'kits/XnuPrelink'))
from coachz_late_init import stage_initializer
from native_prelink import macho

KIT = ROOT / 'kits/coachz'
OUT = ROOT / 'build/coachz-wifi-net'
OUT.mkdir(parents=True, exist_ok=True)
# Our class, the original base, and the tables to check.
OWNED = {'IO80211Interface': 'IONetworkInterface', 'CoachzNetworkStack': 'IONetworkStack'}


def require(value, why):
    if not value:
        raise SystemExit('build-coachz-wifi-net: ' + why)


def run(command, **kwargs):
    return subprocess.run([str(part) for part in command], check=True, **kwargs)


record = json.loads((ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json').read_text().strip().rstrip(','))
arguments, command, index = record['arguments'], [], 0
while index < len(arguments):
    value = arguments[index]
    if value == record['file']:
        command.append(str(KIT / 'CoachzWifiNet.cpp'))
    elif value in ('-o', '-MF', '-MT', '-MJ'):
        if value == '-o':
            command += ['-o', str(OUT / 'CoachzWifiNet.o')]
        index += 1
    elif value == '-Xclang' and arguments[index + 1].startswith('-W'):
        index += 1
    elif not value.startswith('-W'):
        command.append(value)
    index += 1
command += ['-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter', '-Wno-unnecessary-virtual-specifier', '-Wno-c99-designator',
            '-Wno-reorder-init-list', '-Wno-deprecated-enum-enum-conversion', '-I', str(ROOT / '.cache/xnu-work/bsd/dev')]
run(command, cwd=record['directory'])

raw = (OUT / 'CoachzWifiNet.o').read_bytes()
image = macho(raw)
symtab = next(offset for c, offset, _ in image['commands'] if c == 2)
sym_off, count, str_off, _ = struct.unpack_from('<4I', raw, symtab + 8)
symbols = []
for i in range(count):
    name_at, kind, _, _, value = struct.unpack_from('<IBBHQ', raw, sym_off + i * 16)
    symbols.append((raw[str_off + name_at:raw.index(b'\0', str_off + name_at)].decode(), value, kind))
relocations = {}
for segment in image['segments']:
    for section in segment['sections']:
        reloc_off, reloc_count = struct.unpack_from('<2I', raw, section['command_offset'] + 56)
        for i in range(reloc_count):
            address, bits = struct.unpack_from('<2I', raw, reloc_off + i * 8)
            if bits >> 27 & 1 and bits >> 28 == 0 and bits >> 25 & 3 == 3:
                relocations[section['va'] + address] = symbols[bits & 0xffffff][0]
original = json.loads((KIT / 'network-original.json').read_text())['classes']
bound = set(re.findall(r'^\.set (\S+),', (KIT / 'network-original-bindings.S').read_text(), re.M))
for ours, base in OWNED.items():
    spec = original[base]
    table = next(value for name, value, _ in symbols if name == f'__ZTV{len(ours)}{ours}')
    names = [relocations.get(table + 16 + i * 8) for i in range(spec['slots'])]
    require(all(names) and table + 16 + spec['slots'] * 8 not in relocations, f'{ours}: exactly {spec["slots"]} compiled slots')
    own = (f'__ZN{len(ours)}{ours}', f'__ZNK{len(ours)}{ours}')
    for slot in spec['overrides']:
        require(names[slot].startswith(own), f'{ours}: slot {slot} is the original {base} override, compiled to {names[slot]}')
    for slot in range(168, spec['slots']):
        if slot in spec['pure']:
            require(names[slot].startswith(own), f'{ours}: pure slot {slot} must be its own')
        else:
            require(names[slot] == f'__ZN{len(base)}{base}11original{slot}Ev' and names[slot] in bound, f'{ours}: slot {slot} reaches the original')
# The stack user client is an original instance whose IOUserClient overrides are taken from this table at run time (slots in
# OriginalIONetworkingABI.h): it must have exactly the IOUserClient slots and own only setProperties and clientClose of them.
client = original['IONetworkStackUserClient']
table = next(value for name, value, _ in symbols if name == '__ZTV28CoachzNetworkStackUserClient')
names = [relocations.get(table + 16 + i * 8) for i in range(client['slots'])]
require(all(names) and table + 16 + client['slots'] * 8 not in relocations, f'CoachzNetworkStackUserClient: exactly {client["slots"]} slots')
owned = [i for i, name in enumerate(names) if name.startswith(('__ZN28CoachzNetworkStackUserClient', '__ZNK28CoachzNetworkStackUserClient'))]
require(owned == [0, 1, 7, 44, 172], 'CoachzNetworkStackUserClient owns ' + str(owned))
require(set(client['overrides']) - {0, 1, 7} == {44, 86, 170, 172, 173}, 'recorded stack client overrides')
undefined = {name for name, _, kind in symbols if kind == 1 and re.search(r'IONetwork(Interface|Stack)', name)}
require(undefined and undefined <= bound, 'unrecorded original imports: ' + ', '.join(sorted(undefined - bound)))
require(not any(re.search(r'IONetwork(Interface|Stack)D[01]Ev$', name) for name in undefined), 'abstract original destructors must not be used')

late = OUT / 'CoachzWifiNet.late.o'
late.write_bytes(stage_initializer(raw, '__GLOBAL__sub_I_CoachzWifiNet.cpp'))
sdk = subprocess.check_output(['xcrun', '--sdk', 'iphoneos', '--show-sdk-path'], text=True).strip()
bindings = OUT / 'network-original-bindings.o'
run(['xcrun', 'clang', '-target', 'arm64-apple-ios17.0', '-isysroot', sdk, '-c', KIT / 'network-original-bindings.S', '-o', bindings])
archive = OUT / 'libCoachzWifiNet.a'
archive.unlink(missing_ok=True)
run(['xcrun', 'ar', 'rcs', archive, late, bindings])
print('Coachz: Wi-Fi network side built on the original IONetworkingFamily classes,', len(undefined), 'original imports; hardware not accepted')
