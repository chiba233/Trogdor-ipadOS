#!/usr/bin/env python3
"""Build the video codec's kernel service, CoachzVenus.cpp (starts when venus.mbn is in the firmware blob), and the VideoToolbox
decoder it serves, AVD.videodecoder (CoachzVideoDecoder.c), which the kernel carries and writes to the Data volume before launchd."""
# SPDX-License-Identifier: BSD-2-Clause-Patent
# The SMMU kit is referenced, not compiled: libCoachzGpu.a already carries its objects, and every archive is force-loaded into the
# same kernel. The decoder's CDHash goes into a trust cache module of its own, as build-coachz-audio.py does for the audio plug-in.
import hashlib
from pathlib import Path
import re
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'kits/XnuPrelink'))
sys.path.insert(0, str(Path(__file__).resolve().parent))
from native_prelink import macho
from coachz_late_init import stage_initializer

KIT = ROOT / 'kits/coachz'
SMMU = ROOT / 'kits/ArmSmmu4K'
OUT = ROOT / 'build/coachz-venus'
OUT.mkdir(parents=True, exist_ok=True)
KIT_CALLS = {'_ArmSmmu4KLivePrepare', '_ArmSmmu4KProgramDisabledContext', '_ArmSmmu4KLiveUpdateContextBatch'}
KERNEL_CALLS = {'_coachz_rpmh_cx_request', '_coachz_venus_bus_vote', '_coachz_venus_fw', '_coachz_venus_fw_bytes', '_armcpu_spi_attach'}


def require(value, why):
    if not value:
        raise SystemExit('build-coachz-venus: ' + why)


def run(command, **kwargs):
    return subprocess.run([str(part) for part in command], check=True, **kwargs)


# The decoder: a dylib at the path VideoToolbox's VTLoadVideoDecoders dlopens, signed ad hoc, its CDHash in a version 2 trust cache module
# of one entry (SHA-256, no flags, no constraint category).
ios = subprocess.check_output(['xcrun', '--sdk', 'iphoneos', '--show-sdk-path'], text=True).strip()
decoder = OUT / 'AVD.videodecoder'
decoder.unlink(missing_ok=True)
run(['xcrun', 'clang', '-target', 'arm64-apple-ios17.0', '-isysroot', ios, '-I', KIT, '-O2', '-Wall', '-Wextra', '-Werror',
     '-Wno-unused-parameter', '-dynamiclib', '-fvisibility=hidden', '-install_name', '/System/Library/VideoDecoders/AVD.videodecoder',
     '-framework', 'CoreFoundation', '-framework', 'CoreMedia', '-framework', 'CoreVideo', '-framework', 'VideoToolbox',
     '-framework', 'IOKit', KIT / 'CoachzVideoDecoder.c', '-o', decoder])
exported = subprocess.check_output(['xcrun', 'nm', '-gU', decoder], text=True).split()
require('_AVDRegister' in exported, 'the decoder must export AVDRegister')
run(['codesign', '--force', '--sign', '-', '--pagesize', '4096', '--identifier', 'org.trogdor.coachz.video', decoder])
run(['codesign', '--verify', '--strict', decoder])
detail = subprocess.run(['codesign', '--display', '--verbose=4', decoder], capture_output=True, text=True, check=True).stderr
cdhash = bytes.fromhex(re.search(r'^CDHash=([0-9a-f]{40})$', detail, re.M).group(1))
identity = hashlib.sha256(b'Coachz video trust cache').digest()[:16]
(OUT / 'trustcache.bin').write_bytes(struct.pack('<I16sI', 2, identity, 1) + cdhash + bytes((2, 0, 0, 0)))
(OUT / 'decoder.S').write_text('\n'.join(['.const_data', '.globl _coachz_video_decoder', '_coachz_video_decoder:',
                                          f'.incbin "{decoder}"', '.balign 8', '.globl _coachz_video_decoder_size',
                                          '_coachz_video_decoder_size:', f'.quad {decoder.stat().st_size}']) + '\n')
run(['xcrun', 'clang', '-target', 'arm64-apple-ios17.0', '-isysroot', ios, '-c', OUT / 'decoder.S', '-o', OUT / 'decoder.o'])

record = __import__('json').loads((ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json').read_text().strip().rstrip(','))
arguments, command, index = record['arguments'], [], 0
while index < len(arguments):
    value = arguments[index]
    if value == record['file']:
        command.append(str(KIT / 'CoachzVenus.cpp'))
    elif value in ('-o', '-MF', '-MT', '-MJ'):
        if value == '-o':
            command += ['-o', str(OUT / 'CoachzVenus.o')]
        index += 1
    elif value == '-Xclang' and arguments[index + 1].startswith('-W'):
        index += 1
    elif not value.startswith('-W'):
        command.append(value)
    index += 1
command += ['-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter', '-Wno-unnecessary-virtual-specifier', '-Wno-c99-designator',
            '-Wno-reorder-init-list', '-Wno-deprecated-enum-enum-conversion', '-Wno-missing-field-initializers', '-I', str(SMMU)]
run(command, cwd=record['directory'])

raw = (OUT / 'CoachzVenus.o').read_bytes()
image = macho(raw)
symtab = next(offset for c, offset, _ in image['commands'] if c == 2)
sym_off, count, str_off, _ = struct.unpack_from('<4I', raw, symtab + 8)
symbols = []
for i in range(count):
    name_at, kind, _, _, value = struct.unpack_from('<IBBHQ', raw, sym_off + i * 16)
    symbols.append((raw[str_off + name_at:raw.index(b'\0', str_off + name_at)].decode(), kind))
undefined = {name for name, kind in symbols if kind == 1}
defined = {name for name, kind in symbols if kind & 0xe == 0xe}
require(KIT_CALLS <= undefined, 'the SMMU kit calls: ' + ', '.join(sorted(KIT_CALLS - undefined)))
require(KERNEL_CALLS <= undefined, 'the kernel side in coachz_wifi.c: ' + ', '.join(sorted(KERNEL_CALLS - undefined)))
require(not any(n.startswith('_ArmSmmu4K') for n in defined), 'the SMMU kit must come from libCoachzGpu.a only')
require('_coachz_venus_start' in defined, 'entry point')
# Its classes register when the service starts, after the original IOKit classes (CoachzAudio.cpp's route).
late = OUT / 'CoachzVenus.late.o'
late.write_bytes(stage_initializer(raw, '__GLOBAL__sub_I_CoachzVenus.cpp'))
archive = OUT / 'libCoachzVenus.a'
archive.unlink(missing_ok=True)
run(['xcrun', 'ar', 'rcs', archive, late, OUT / 'decoder.o'])
print('Coachz: video decode service and VideoToolbox decoder', cdhash.hex(), 'built; hardware not accepted')
