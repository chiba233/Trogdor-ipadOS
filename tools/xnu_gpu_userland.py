#!/usr/bin/env python3
"""Pack the Metal plug-in and Turnip for coachz-gpu=9: the addon file the kernel installs on the Data volume, and the trust cache module for their code."""
# SPDX-License-Identifier: BSD-2-Clause
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[1]
# The GPU userland comes from its own UniXNU checkout, newer than the one the kernel is built from (upstream.lock.json "unixnu-gpu"):
# its render server encodes on a thread of its own. The kernel keeps the pinned Adreno740 framework, which patches/gpu/Adreno740.patch
# gives the newer userland's two extra methods.
UNIXNU = ROOT / 'upstream/unixnu-gpu'


def verify():
    pinned = json.loads((ROOT / 'upstream.lock.json').read_text())['sources']['unixnu-gpu']['commit']
    revision = subprocess.check_output(['git', '-C', str(UNIXNU), 'rev-parse', 'HEAD'], text=True).strip()
    dirty = subprocess.check_output(['git', '-C', str(UNIXNU), 'status', '--porcelain', '--untracked-files=no'], text=True)
    if revision != pinned or dirty:
        raise SystemExit('upstream/unixnu-gpu must be the clean pinned checkout')


def build():
    # Turnip for the tablet and for the host (the plug-in build records Turnip's first submission on the host), then the plug-in. The Mesa work tree
    # (.cache/mesa-work) carries the pinned Mesa with the recorded patches; build-turnip-darwin.py checks both.
    for command in (['scripts/build-turnip-darwin.py', '--target', 'ios'], ['scripts/build-turnip-darwin.py', '--target', 'host']):
        subprocess.run(['python3', '-B', *command], cwd=UNIXNU, check=True)
    # The plug-in build replays Turnip's first submission on a host simulation of the kernel driver (build/turnip-darwin-host). It comes from the
    # pinned kernel tree, whose Adreno740 framework is the one the tablet runs; the newer framework's sources no longer build on the host.
    pinned = ROOT / 'upstream/unixnu'
    harness = pinned / 'build/turnip-darwin-host'
    if not (harness / 'plugin_smoke').exists():
        subprocess.run(['python3', '-B', 'scripts/check-turnip-darwin-host.py'], cwd=pinned, check=True)
    target = UNIXNU / 'build/turnip-darwin-host'
    shutil.rmtree(target, ignore_errors=True)
    shutil.copytree(harness, target)
    # patches/gpu/unixnu-gpu-plugin.patch, applied only while the plug-in builds: the pinned plug-in does not build for iOS 17 as it is.
    patch = ['patch', '--batch', '--silent', '-p1', '-d', str(UNIXNU / 'drivers/AdrenoMetal'), '-i',
             str(ROOT / 'platform/coachz-prelink/patches/gpu/unixnu-gpu-plugin.patch')]
    subprocess.run(patch, check=True)
    try:
        subprocess.run(['python3', '-B', 'scripts/build-adreno-metal-plugin.py'], cwd=UNIXNU, check=True)
    finally:
        subprocess.run([*patch, '-R'], check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT / 'build/coachz-gpu-userland')
    parser.add_argument('--build', action='store_true', help='build Turnip and the plug-in first')
    args = parser.parse_args()
    verify()
    if args.build:
        build()
    args.output.mkdir(parents=True, exist_ok=True)
    addon = args.output / 'NativeAdrenoMetal.addon'
    addon.unlink(missing_ok=True)
    subprocess.run(['python3', '-B', 'scripts/make-gpu-addon.py', '--publish', str(addon)], cwd=UNIXNU, check=True)
    trust = json.loads((UNIXNU / 'build/adreno-metal-plugin/trust.json').read_text())
    # Every signed executable the build reports (the plug-in, Turnip, the probe daemon). A version 2 module, as build-coachz-audio.py writes:
    # SHA-256 (hash type 2), no flags, no constraint category, sorted by hash.
    hashes = sorted(bytes.fromhex(value) for key, value in trust.items() if key.endswith('cdhash'))
    if len(hashes) < 3:
        raise SystemExit('trust.json lacks the plug-in, Turnip or daemon hash')
    identity = hashlib.sha256(b'Coachz GPU trust cache').digest()[:16]
    (args.output / 'trustcache.bin').write_bytes(struct.pack('<I16sI', 2, identity, len(hashes)) + b''.join(h + bytes((2, 0, 0, 0)) for h in hashes))
    print('addon', addon.stat().st_size, 'bytes,', len(hashes), 'trusted hashes')


if __name__ == '__main__':
    main()
