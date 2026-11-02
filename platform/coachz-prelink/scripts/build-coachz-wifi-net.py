#!/usr/bin/env python3
"""Build the Wi-Fi network side, the Ethernet interface and Apple80211 requests of CoachzWifiNet.cpp, for the kernel."""
# SPDX-License-Identifier: BSD-2-Clause-Patent
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(Path(__file__).resolve().parent))
from coachz_late_init import stage_initializer

OUT = ROOT / 'build/coachz-wifi-net'
OUT.mkdir(parents=True, exist_ok=True)


def run(command, **kwargs):
    return subprocess.run([str(part) for part in command], check=True, **kwargs)


record = json.loads((ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json').read_text().strip().rstrip(','))
arguments, command, index = record['arguments'], [], 0
while index < len(arguments):
    value = arguments[index]
    if value == record['file']:
        command.append(str(ROOT / 'kits/coachz/CoachzWifiNet.cpp'))
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
late = OUT / 'CoachzWifiNet.late.o'
late.write_bytes(stage_initializer((OUT / 'CoachzWifiNet.o').read_bytes(), '__GLOBAL__sub_I_CoachzWifiNet.cpp'))
archive = OUT / 'libCoachzWifiNet.a'
archive.unlink(missing_ok=True)
run(['xcrun', 'ar', 'rcs', archive, late])
print('Coachz: Wi-Fi network side built; hardware not accepted')
