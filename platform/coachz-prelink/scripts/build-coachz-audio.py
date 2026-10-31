#!/usr/bin/env python3
"""Build the Coachz audio server plug-in as a signed driver bundle and the kernel audio service that carries it."""
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Plug-in build adapted from UniXNU-sheng scripts/build-sheng-audio-hal.py, revision 08cde20. The iOS SDK has no audio server plug-in
# headers; the macOS SDK's CoreAudio headers declare the same interface and are used for the build only. The bundle's files go into the
# kernel, which writes them to the Data volume before launchd, and the executable's CDHash goes into a trust cache module of its own.
import hashlib
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(Path(__file__).resolve().parent))
from coachz_late_init import stage_initializer

KIT = ROOT / 'kits/coachz'
OUT = ROOT / 'build/coachz-audio'
OUT.mkdir(parents=True, exist_ok=True)


def run(command, **kwargs):
    return subprocess.run([str(part) for part in command], check=True, **kwargs)


ios = subprocess.check_output(['xcrun', '--sdk', 'iphoneos', '--show-sdk-path'], text=True).strip()
mac = subprocess.check_output(['xcrun', '--sdk', 'macosx', '--show-sdk-path'], text=True).strip()
bundle = OUT / 'CoachzAudio.driver'
shutil.rmtree(bundle, ignore_errors=True)
bundle.mkdir()
with tempfile.TemporaryDirectory() as directory:
    headers = Path(directory) / 'CoreAudio'
    headers.mkdir()
    for name in ('AudioServerPlugIn.h', 'AudioHardwareBase.h', 'AudioHardware.h'):
        (headers / name).symlink_to(Path(mac) / 'System/Library/Frameworks/CoreAudio.framework/Headers' / name)
    run(['xcrun', 'clang', '-target', 'arm64-apple-ios17.0', '-isysroot', ios, '-I', directory, '-I', KIT, '-O2', '-Wall', '-Wextra', '-Werror',
         '-bundle', '-fvisibility=hidden', '-framework', 'CoreFoundation', '-framework', 'IOKit', KIT / 'CoachzAudioPlugIn.c',
         '-o', bundle / 'CoachzAudio'])
shutil.copy(KIT / 'CoachzAudioPlugIn.plist', bundle / 'Info.plist')
run(['codesign', '--force', '--sign', '-', '--pagesize', '4096', '--identifier', 'org.trogdor.coachz.audio', bundle])
run(['codesign', '--verify', '--strict', bundle])
detail = run(['codesign', '--display', '--verbose=4', bundle], capture_output=True, text=True).stderr
cdhash = bytes.fromhex(re.search(r'^CDHash=([0-9a-f]{40})$', detail, re.M).group(1))
# A version 2 trust cache module of one entry: SHA-256 (hash type 2), no flags, no constraint category.
identity = hashlib.sha256(b'Coachz audio trust cache').digest()[:16]
(OUT / 'trustcache.bin').write_bytes(struct.pack('<I16sI', 2, identity, 1) + cdhash + bytes((2, 0, 0, 0)))

files = {'exec': bundle / 'CoachzAudio', 'plist': bundle / 'Info.plist', 'resources': bundle / '_CodeSignature/CodeResources'}
assembly = ['.const_data']
for name, path in files.items():
    assembly += [f'.globl _coachz_audio_bundle_{name}', f'_coachz_audio_bundle_{name}:', f'.incbin "{path}"', '.balign 8',
                 f'.globl _coachz_audio_bundle_{name}_size', f'_coachz_audio_bundle_{name}_size:', f'.quad {path.stat().st_size}']
(OUT / 'bundle.S').write_text('\n'.join(assembly) + '\n')
run(['xcrun', 'clang', '-target', 'arm64-apple-ios17.0', '-isysroot', ios, '-c', OUT / 'bundle.S', '-o', OUT / 'bundle.o'])

# The service, compiled like the display glue with the platform expert's flags; its classes register once LPASS is ready.
record = json.loads((ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json').read_text().strip().rstrip(','))
arguments, command, index = record['arguments'], [], 0
while index < len(arguments):
    value = arguments[index]
    if value == record['file']:
        command.append(str(KIT / 'CoachzAudio.cpp'))
    elif value in ('-o', '-MF', '-MT', '-MJ'):
        if value == '-o':
            command += ['-o', str(OUT / 'CoachzAudio.o')]
        index += 1
    elif value == '-Xclang' and arguments[index + 1].startswith('-W'):
        index += 1
    elif not value.startswith('-W'):
        command.append(value)
    index += 1
command += ['-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter', '-Wno-unnecessary-virtual-specifier', '-Wno-c99-designator',
            '-Wno-reorder-init-list', '-Wno-deprecated-enum-enum-conversion']
run(command, cwd=record['directory'])
late = OUT / 'CoachzAudio.late.o'
late.write_bytes(stage_initializer((OUT / 'CoachzAudio.o').read_bytes(), '__GLOBAL__sub_I_CoachzAudio.cpp'))
archive = OUT / 'libCoachzAudio.a'
archive.unlink(missing_ok=True)
run(['xcrun', 'ar', 'rcs', archive, late, OUT / 'bundle.o'])
print('Coachz: audio plug-in', cdhash.hex(), 'and service built; hardware not accepted')
