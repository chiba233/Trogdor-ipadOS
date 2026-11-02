#!/usr/bin/env python3
"""Build the A618 kernel GPU driver: the Adreno740 framework with its A618 patch, the SMMU and PM4 kits, the staged accelerator and the board code."""
# SPDX-License-Identifier: MIT
# Adapted from UniXNU-sheng scripts/build-adreno740-kits.py and UniXNU scripts/build-adreno740-accelerator.py (MIT): the Sheng GMU kits are left out,
# because coachz_gpu_seq.h starts the GMU here, and with them the files that use them (NativeAdrenoService, GmuConfig, GmuPlatform).
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(Path(__file__).resolve().parent))
from coachz_late_init import stage_initializer

KIT = ROOT / 'kits/coachz'
DRIVER = ROOT / 'drivers/Adreno740'
SMMU = ROOT / 'kits/ArmSmmu4K'
PM4 = ROOT / 'kits/AdrenoPM4'
OUT = ROOT / 'build/coachz-gpu'
OUT.mkdir(parents=True, exist_ok=True)
UNIXNU = Path(os.environ['COACHZ_UNIXNU'])


def recipe(path):
    return json.loads((ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837' / path).read_text().strip().rstrip(','))


def compile_object(record, source, output, extra):
    arguments, command, index = record['arguments'], [], 0
    while index < len(arguments):
        value = arguments[index]
        if value == record['file']:
            command.append(str(source))
        elif value in ('-o', '-MF', '-MT', '-MJ'):
            if value == '-o':
                command += ['-o', str(output)]
            index += 1
        elif value == '-Xclang' and arguments[index + 1].startswith('-W'):
            index += 1
        elif not value.startswith('-W') and value not in ('-MD', '-MP'):
            command.append(value)
        index += 1
    subprocess.run(command + extra, cwd=record['directory'], check=True)


includes = ['-I', str(DRIVER), '-I', str(SMMU), '-I', str(PM4), '-I', str(ROOT / '.cache/xnu-work/osfmk')]
c_record, cxx_record = recipe('bsd/DEVELOPMENT/bsd_init.o.json'), recipe('iokit/DEVELOPMENT/IOPlatformExpert.cpo.json')
objects = []
for source in [PM4 / 'AdrenoPM4.c', *sorted(p for p in SMMU.glob('ArmSmmu4K*.c'))]:
    output = OUT / (source.stem + '.kit.o')
    compile_object(c_record, source, output, [*includes, '-w'])
    objects.append(output)
# NativeAdrenoGmuMemory is the allocator type the GPU context's header carries; everything else that needs the Sheng GMU stays out.
for stem in ['Cpu', 'GmuMemory', 'GpuContext', 'Memory', 'Preempt', 'Publish', 'Ring', 'SmmuContext', 'SmmuMemory', 'Submit', 'Timer', 'UserClient', 'Vm']:
    output = OUT / ('NativeAdreno' + stem + '.o')
    compile_object(cxx_record, DRIVER / ('NativeAdreno' + stem + '.cpp'), output, [*includes, '-w'])
    objects.append(output)
# The scanout hand-off the Metal plug-in serves: CoachzDisplay.cpp creates it with sheng-fb-gpu=1.
presenter = OUT / 'NativeGOPPresenter.o'
compile_object(cxx_record, ROOT / 'drivers/GopFramebuffer/NativeGOPPresenter.cpp', presenter, [*includes, '-I', str(ROOT / 'drivers/GopFramebuffer'), '-w'])
objects.append(presenter)
board = OUT / 'CoachzGpu.o'
compile_object(cxx_record, KIT / 'CoachzGpu.cpp', board, [*includes, '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
               '-Wno-unnecessary-virtual-specifier', '-Wno-c99-designator', '-Wno-reorder-init-list', '-Wno-deprecated-enum-enum-conversion',
               '-Wno-cast-qual', '-Wno-format'])
objects.append(board)

# The accelerator derives from the original IOAcceleratorES, bound by the addresses of the audited kernelcache; its initializer runs from the board
# code, after IOGPUFamily has loaded, because the parent metaclass does not exist before that.
plan = json.loads((UNIXNU / 'scripts/data/ioaccelerator-base-abi.json').read_text())
if plan['operational_overrides'] or plan['instance_bytes'] != 136 or plan['virtual_entries'] != 168:
    sys.exit('The audited original base is not the expected 136-byte, 168-entry class without operational overrides')
accelerator = OUT / 'NativeAdrenoAccelerator.o'
compile_object(cxx_record, DRIVER / 'NativeAdrenoAccelerator.cpp', accelerator, [*includes, '-w'])
late = OUT / 'NativeAdrenoAccelerator.late.o'
late.write_bytes(stage_initializer(accelerator.read_bytes(), '__GLOBAL__sub_I_NativeAdrenoAccelerator.cpp'))
bindings = {'__ZN15IOAcceleratorESC2EPK11OSMetaClass': int(plan['protected_compatible_constructor_aliases'][0], 16),
            '__ZN15IOAcceleratorESD2Ev': int(plan['nondeleting_destructor'], 16),
            '__ZN15IOAcceleratorES10gMetaClassE': int(plan['metaclass'], 16)}
assembly = OUT / 'original-bindings.S'
assembly.write_text(''.join(f'.globl {name}\n.set {name}, {address:#x}\n' for name, address in sorted(bindings.items())))
sdk = subprocess.check_output(['xcrun', '--sdk', 'iphoneos', '--show-sdk-path'], text=True).strip()
subprocess.run(['xcrun', 'clang', '-target', 'arm64-apple-ios17.0', '-isysroot', sdk, '-c', str(assembly), '-o', str(OUT / 'original-bindings.o')], check=True)
objects += [late, OUT / 'original-bindings.o']

archive = OUT / 'libCoachzGpu.a'
archive.unlink(missing_ok=True)
subprocess.run(['xcrun', 'ar', 'rcs', str(archive), *map(str, objects)], check=True)
print('Coachz: A618 driver archive with', len(objects), 'objects; hardware not accepted')
