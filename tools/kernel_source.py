#!/usr/bin/env python3
"""Prepare/build the pinned UniXNU source kernel without UEFI or image downloads."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
COMPONENTS = ('drivers/ArmCpu', 'drivers/ArmDTPlatform', 'drivers/SoftwareAES', 'kits/XnuPrelink')
REFERENCES = {'xnu', 'libplatform', 'libdispatch', 'AvailabilityVersions', 'libpthread',
              'corecrypto-2023', 'IOStorageFamily'}


def verify(name):
    spec = json.loads((ROOT / 'upstream.lock.json').read_text())['sources'][name]
    path = ROOT / 'upstream' / name
    revision = subprocess.check_output(['git', '-C', str(path), 'rev-parse', 'HEAD'], text=True).strip()
    dirty = subprocess.check_output(['git', '-C', str(path), 'status', '--porcelain',
                                     '--untracked-files=no'], text=True)
    if revision != spec['commit'] or dirty:
        raise ValueError(name + ': checkout must match the clean pinned source')
    return path


def materialize_xnu(source, destination):
    """Make a single-revision, non-promisor reference for upstream shared clones."""
    wanted = subprocess.check_output(['git', '-C', str(source), 'rev-parse', 'HEAD'], text=True).strip()
    if not destination.exists():
        destination.mkdir(parents=True)
        subprocess.run(['git', 'init', '--quiet', str(destination)], check=True)
        subprocess.run(['git', '-C', str(destination), 'fetch', '--quiet', '--no-tags',
                        '--depth=1', str(source), wanted], check=True)
        subprocess.run(['git', '-C', str(destination), 'checkout', '--quiet', '--detach', wanted], check=True)
    current = subprocess.check_output(['git', '-C', str(destination), 'rev-parse', 'HEAD'], text=True).strip()
    dirty = subprocess.check_output(['git', '-C', str(destination), 'status', '--porcelain'], text=True)
    if current != wanted or dirty:
        raise ValueError('cached XNU reference differs from the pinned clean revision')
    return destination


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--build', action='store_true', help='also compile the source kernel; no prelink or boot')
    p.add_argument('--jobs', type=int, default=min(os.cpu_count() or 2, 8))
    p.add_argument('--output', type=Path, default=ROOT / 'build/xnu-source')
    a = p.parse_args()
    out = a.output.resolve()
    if a.jobs < 1 or out.is_relative_to(Path('/dev')):
        p.error('positive job count and ordinary output directory required')
    base, xnu = verify('unixnu'), verify('apple-xnu')
    xnu = materialize_xnu(xnu, out / 'reference/xnu')
    subprocess.run(['git', '-C', str(base), 'submodule', 'update', '--init', '--depth=1',
                    *COMPONENTS], check=True)
    # Use only the selected source components. The normal CLI also initializes all
    # unrelated module submodules and prelinks/downloads IPSW input after a build.
    sys.path.insert(0, str(base / 'scripts'))
    from unixnu import cli, feeds, kernel, tree
    from unixnu.config import Context
    arguments = cli.parser().parse_args(['defconfig', 'coachz_defconfig', '-D',
        str(ROOT / 'platform/coachz'), '--out', str(out), '-j', str(a.jobs),
        '--local-module', 'refer:xnu=' + str(xnu)])
    ctx = Context(arguments)
    cli.cmd_defconfig(ctx)
    # cmd_defconfig writes .config; Context keeps the original in-memory snapshot.
    ctx = Context(arguments)
    modules = feeds.resolve(ctx)
    wanted = {m.name for m in modules.values()
              if m.origin == 'git' and (m.kind != 'refer' or m.name in REFERENCES)}
    feeds.fetch(ctx, modules, only=wanted)
    tree.assemble(ctx, modules)
    sources = {}
    for (kind, name), module in modules.items():
        if module.path is not None and kind in ('refer', 'driver', 'kit'):
            revision = subprocess.check_output(['git', '-C', str(module.path), 'rev-parse', 'HEAD'],
                                               text=True).strip()
            sources[kind + ':' + name] = revision
    report = {'schema': 1, 'unixnu_revision': json.loads((ROOT / 'upstream.lock.json').read_text())[
        'sources']['unixnu']['commit'], 'sources': sources, 'darwin': '23.6.0',
        'xnu_version': '10063.141.1', 'page_size': 4096,
        'config_sha256': hashlib.sha256(ctx.config_path.read_bytes()).hexdigest(),
        'source_kernel_built': False, 'prelinked': False, 'hardware_boot_tested': False}
    report_path = out / 'source-preparation.json'
    report_path.write_text(json.dumps(report, indent=2) + '\n')
    if a.build:
        local_tools = ROOT / '.tools/xnu-host-deps'
        bins = [local_tools / 'cmake/data/bin', local_tools / 'bin']
        os.environ['PATH'] = os.pathsep.join([*(str(b) for b in bins if b.is_dir()), os.environ.get('PATH', '')])
        kernel.build(ctx, modules)
        candidate = ctx.tree / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837.unstripped.noctf'
        if not candidate.is_file():
            raise ValueError('build returned without the expected source kernel')
        report['source_kernel_built'] = True
        report['source_kernel_sha256'] = hashlib.sha256(candidate.read_bytes()).hexdigest()
        report_path.write_text(json.dumps(report, indent=2) + '\n')
    print('Source preparation complete; report:', report_path)
    print('No prelinked kernel, hardware write or kernel boot is implied.')


if __name__ == '__main__':
    main()
