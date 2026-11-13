#!/usr/bin/env python3
"""Pack the Metal plug-in and Turnip for coachz-gpu=9: the addon file the kernel installs on the Data volume, and the trust cache module for their code."""
# SPDX-License-Identifier: BSD-2-Clause
import argparse
from contextlib import contextmanager
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


@contextmanager
def gpu_stack_overlay():
    """Keep the pinned checkout and its recorded Mesa/runtime patch sets intact."""
    stack = ROOT / 'platform/coachz-prelink/patches/gpu/unixnu-gpu-stack.patch'
    mesa = UNIXNU / '.cache/mesa-work'
    runtime = UNIXNU / '.cache/gpu-reference/metal2vulkan'
    # The overlay's Mesa patches in the order adreno-compiler-inputs.json records them: UniXNU's transient attachments in GMEM with lazily
    # allocated memory under TU_DEBUG=sysmem (b95b4c9) and the memory-side colour swap of GMEM transfers (93ad2ce), then the diagnostics,
    # then later UniXNU fixes on the same Mesa commit (85fda3b): 215672b fragment consts that leave the other stages room (no safe_constlen
    # variant), 97437dc one GPU address allocator per process, c72a8a5 the device kept after a GPU restart, 6061e10 the row pitch for padding,
    # d76f17d long submissions cut where a command buffer begins.
    mesa_patches = [UNIXNU / 'patches/mesa' / name for name in ('turnip-transient-attachments-in-gmem.patch',
                                                                 'turnip-gmem-memory-side-swap.patch', 'turnip-coachz-diagnostics.patch',
                                                                 'coachz-port-ir3-fragment-consts.patch', 'coachz-port-turnip-one-va.patch',
                                                                 'coachz-port-turnip-keep-device.patch', 'coachz-port-turnip-row-pitch.patch',
                                                                 'coachz-port-turnip-submit-cuts.patch')]
    runtime_patch = UNIXNU / 'drivers/AdrenoMetal/bridge/patches/0026-coachz-copy-profile.patch'
    def apply(tree, patch, reverse=False, check=False):
        command = ['git', '-C', str(tree), 'apply']
        if reverse:
            command.append('--reverse')
        if check:
            command.append('--check')
        return subprocess.run([*command, str(patch)], check=not check, capture_output=check)
    # git apply is atomic: a rejected overlay leaves the tree as it was.
    apply(UNIXNU, stack)
    mesa_applied = []
    try:
        for patch in mesa_patches:
            apply(mesa, patch)
            mesa_applied.append(patch)
        yield
    finally:
        # The bridge builder applies its exact recorded patch sequence. Remove this
        # sequence's last patch only if the builder reached it, including on failure.
        try:
            if apply(runtime, runtime_patch, reverse=True, check=True).returncode == 0:
                apply(runtime, runtime_patch, reverse=True)
        finally:
            try:
                for patch in reversed(mesa_applied):
                    apply(mesa, patch, reverse=True)
            finally:
                apply(UNIXNU, stack, reverse=True)


def require_substantive_batch():
    """Reject an unfinished active GPU batch before applying or building anything."""
    manifest = ROOT / '.tools/handoff/gpu-active-batch.json'
    if not manifest.exists():
        return
    batch = json.loads(manifest.read_text())
    if not batch.get('in_progress'):
        return
    changes = batch.get('runtime_changes', [])
    ids = {item.get('id') for item in changes if isinstance(item, dict) and item.get('id')}
    minimum = max(8, int(batch.get('minimum_substantive_changes', 8)))
    count = int(batch.get('implemented_substantive_changes', 0))
    if count < minimum or len(changes) != count or len(ids) != count:
        raise SystemExit(f'GPU build refused: active batch has {count}/{minimum} substantive changes; diagnostics and old changes do not count')
    if batch.get('diagnostics_count_as_improvements') is not False or batch.get('old_changes_count_as_new') is not False:
        raise SystemExit('GPU build refused: active batch must exclude diagnostics and old changes from its substantive count')


def build(prime_list=None):
    require_substantive_batch()
    with gpu_stack_overlay():
        build_overlaid(prime_list)


def build_overlaid(prime_list=None):
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
    patch = ['git', '-C', str(UNIXNU), 'apply', '--directory=drivers/AdrenoMetal',
             str(ROOT / 'platform/coachz-prelink/patches/gpu/unixnu-gpu-plugin.patch')]
    # The plug-in build embeds build/prime-list.txt in __DATA,__primelist when it exists (build-adreno-metal-plugin.py).
    embedded = UNIXNU / 'build/prime-list.txt'
    embedded.unlink(missing_ok=True)
    if prime_list:
        embedded.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(prime_list, embedded)
    subprocess.run(patch, check=True)
    try:
        subprocess.run(['python3', '-B', 'scripts/build-adreno-metal-plugin.py'], cwd=UNIXNU, check=True)
    finally:
        subprocess.run([*patch[:-1], '--reverse', patch[-1]], check=True)


# The default wallpaper's image layers as JPEG instead of HEIC (no hardware HEVC decoder here), as UniXNU's scripts/make-wallpaper-overlay.py makes them:
# the animation's documents with main.caml pointing at the new images next to it. JPEG keeps them small enough for the kernel partition.
WALLPAPERS = 'Library/Wallpaper/Collections/iPadOS_17~ipad.wallpaperCollection/Wallpapers/'
WALLPAPER_OVERLAYS = {
    'wallpaper': (WALLPAPERS + '7570.iPadOS_17_Flat-1024w-1366h@2x~ipad.wallpaper/7570.iPadOS_17_Flat_Background-1024w-1366h@2x~ipad.ca', {'J120': 'J120', 'J120-Dark': 'J120-Dark'}),
    'wallpaper-floating': (WALLPAPERS + '7570.iPadOS_17-834w-1112h@2x~ipad.wallpaper/7570.iPadOS_17_Background-834w-1112h@2x~ipad.ca',
                           {'Background%20Light': 'Background-Light', 'Background%20Dark': 'Background-Dark'}),
}


def wallpaper_overlay(root, output, animation, images):
    source = root / animation
    if not (source / 'main.caml').is_file():
        raise SystemExit('no wallpaper animation at ' + str(source))
    shutil.rmtree(output, ignore_errors=True)
    output.mkdir(parents=True)
    for name in ('index.xml', 'assetManifest.caml'):
        (output / name).write_bytes((source / name).read_bytes())
    text = (source / 'main.caml').read_text()
    for image, jpeg in images.items():
        text = text.replace(f'/assets/{image}.HEIC', f'/{jpeg}.jpg').replace(f'assets/{image}.HEIC', f'{jpeg}.jpg')
        heic = source / 'assets' / (image.replace('%20', ' ') + '.HEIC')
        subprocess.run(['sips', '-Z', '2732', '-s', 'format', 'jpeg', '-s', 'formatOptions', '85', str(heic), '--out', str(output / f'{jpeg}.jpg')],
                       check=True, stdout=subprocess.DEVNULL)
    if '.HEIC"/>' in text:   # a layer's name may keep it
        raise SystemExit('main.caml still points at a HEIC image')
    (output / 'main.caml').write_text(text)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT / 'build/coachz-gpu-userland')
    parser.add_argument('--build', action='store_true', help='build Turnip and the plug-in first')
    parser.add_argument('--make-prime-list', type=Path, nargs='+', metavar='ROOT',
                        help='write the prime list from these mounted OS roots (the OS image, its SystemOS cryptex) to --prime-list and stop')
    parser.add_argument('--wallpaper-root', type=Path, help='the mounted OS volume: carry the default wallpaper with JPEG images in place of its HEIC ones')
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
    if args.wallpaper_root:
        for name, (animation, images) in WALLPAPER_OVERLAYS.items():
            directory = args.output / name
            wallpaper_overlay(args.wallpaper_root, directory, animation, images)
            extra += ['--' + name, str(directory.resolve())]
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
