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


# The libraries the render server compiles ahead while the screen is idle (docs: porting/gpu#shader-warmup), as it sees them: the absolute paths of
# every .metallib in the OS image and its SystemOS cryptex, which the union mounts at the same paths. Ordered as asteroids T75 found best:
# QuartzCore, then CoreImage, the other public frameworks, private frameworks, the rest of /System, applications last; MTLReplayer's ray-tracing
# kernels are left out (T74 spent 88 s on them and never reached QuartzCore), and so is /private/var: the external Data volume replaces it at
# run time and holds no staged_system_apps.
PRIME_SKIP = ('/Applications/MTLReplayer.app/', '/private/')


def prime_rank(path):
    if path == '/System/Library/Frameworks/QuartzCore.framework/default.metallib':
        return 0
    for rank, prefix in enumerate(('/System/Library/Frameworks/CoreImage.framework/', '/System/Library/Frameworks/',
                                   '/System/Library/PrivateFrameworks/', '/System/'), 1):
        if path.startswith(prefix):
            return rank
    return 5


def make_prime_list(roots, output):
    paths = set()
    for root in roots:
        for library in root.rglob('*.metallib'):
            if library.is_file() and not library.is_symlink():
                paths.add('/' + library.relative_to(root).as_posix())
    paths = sorted((p for p in paths if not p.startswith(PRIME_SKIP)), key=lambda p: (prime_rank(p), p))
    if not paths or paths[0] != '/System/Library/Frameworks/QuartzCore.framework/default.metallib':
        raise SystemExit('the roots have no QuartzCore library; mount the OS image (and its SystemOS cryptex) first')
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(''.join(p + '\n' for p in paths))
    print('prime list', len(paths), 'libraries')


def build(prime_list=None):
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
    # The plug-in build embeds build/prime-list.txt in __DATA,__primelist when it exists (build-adreno-metal-plugin.py).
    embedded = UNIXNU / 'build/prime-list.txt'
    embedded.unlink(missing_ok=True)
    if prime_list:
        embedded.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(prime_list, embedded)
    try:
        subprocess.run(['python3', '-B', 'scripts/build-adreno-metal-plugin.py'], cwd=UNIXNU, check=True)
    finally:
        subprocess.run([*patch, '-R'], check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT / 'build/coachz-gpu-userland')
    parser.add_argument('--build', action='store_true', help='build Turnip and the plug-in first')
    parser.add_argument('--make-prime-list', type=Path, nargs='+', metavar='ROOT',
                        help='write the prime list from these mounted OS roots (the OS image, its SystemOS cryptex) to --prime-list and stop')
    parser.add_argument('--prime-list', type=Path, help='the libraries the render server compiles ahead, embedded in the plug-in and carried in the addon')
    args = parser.parse_args()
    if args.make_prime_list:
        if not args.prime_list:
            raise SystemExit('--make-prime-list needs --prime-list for its output')
        make_prime_list(args.make_prime_list, args.prime_list)
        return
    verify()
    if args.build:
        build(args.prime_list)
    args.output.mkdir(parents=True, exist_ok=True)
    addon = args.output / 'NativeAdrenoMetal.addon'
    addon.unlink(missing_ok=True)
    extra = ['--prime-list', str(args.prime_list.resolve())] if args.prime_list else []
    subprocess.run(['python3', '-B', 'scripts/make-gpu-addon.py', '--publish', *extra, str(addon)], cwd=UNIXNU, check=True)
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
