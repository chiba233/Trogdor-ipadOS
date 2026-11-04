#!/usr/bin/env python3
"""Stage an opt-in rootless app environment alongside an unchanged GPU addon.

Packages remain local inputs; no Apple binary or third-party package is committed.
Procursus: https://apt.procurs.us/bootstraps/1900/
Sileo: https://github.com/Sileo/Sileo/releases
TrollStore Lite: https://havoc.app/package/trollstorelite
"""
import argparse
import gzip
import hashlib
import io
import json
import os
from pathlib import Path
import plistlib
import shutil
import struct
import subprocess
import tarfile
import zlib

ROOT = Path(__file__).resolve().parent.parent
BOOT_ENTRY_TARGET = '/private/var/ngpu/owner-apps/coachz-apps'


def launchd_manifest(stock):
    """Add one direct system job; keep every original job and all other metadata."""
    result = plistlib.loads(stock)
    job = {'Label': 'com.coachz.apps', 'ProgramArguments': [BOOT_ENTRY_TARGET, 'bootstrap'],
           'RunAtLoad': True, 'UserName': 'root', 'ProcessType': 'Background',
           'KeepAlive': {'SuccessfulExit': False}, 'ThrottleInterval': 30,
           'com.apple.private.security.no-sandbox': True,
           'StandardOutPath': '/private/var/log/coachz-apps.log',
           'StandardErrorPath': '/private/var/log/coachz-apps.log'}
    # The same OS manifest uses this spelling on its original replayd job.
    assert result['LaunchDaemons']['/System/Library/LaunchDaemons/com.apple.replayd.plist']['com.apple.private.security.no-sandbox'] is True
    path = '/System/Library/LaunchDaemons/com.coachz.apps.plist'
    assert path not in result['LaunchDaemons']
    result['LaunchDaemons'][path] = job
    packed = plistlib.dumps(result, fmt=plistlib.FMT_BINARY, sort_keys=False)
    check = plistlib.loads(packed)
    assert check['LaunchDaemons'].pop(path) == job
    assert check == plistlib.loads(stock), 'an original launchd entry changed'
    return packed, plistlib.dumps(job)


def cdhashes(data):
    """All ARM64 CodeDirectories, bounded by their Mach-O slice and signature."""
    if len(data) < 4:
        return set()
    if data[:4] in (b'\xca\xfe\xba\xbe', b'\xca\xfe\xba\xbf'):
        wide = data[3] == 0xbf
        n, = struct.unpack_from('>I', data, 4)
        stride = 32 if wide else 20
        assert 0 < n <= 64 and 8 + n * stride <= len(data)
        result = set()
        for i in range(n):
            at = 8 + stride * i
            cpu, = struct.unpack_from('>I', data, at)
            off, size = struct.unpack_from('>QQ' if wide else '>II', data, at + 8)
            assert off + size <= len(data)
            if cpu == 0x100000c:
                result |= cdhashes(data[off:off + size])
        return result
    if data[:4] != b'\xcf\xfa\xed\xfe':
        return set()
    n, size = struct.unpack_from('<II', data, 16)
    assert 32 + size <= len(data) and n <= size // 8
    at = 32
    for _ in range(n):
        cmd, length = struct.unpack_from('<II', data, at)
        assert 8 <= length <= 32 + size - at
        if cmd == 0x1d:
            off, size = struct.unpack_from('<II', data, at + 8)
            assert off + size <= len(data)
            blob = data[off:off + size]
            magic, total, count = struct.unpack_from('>III', blob)
            assert magic == 0xfade0cc0 and total <= size and count <= (total - 12) // 8
            out = set()
            for i in range(count):
                _, cd = struct.unpack_from('>II', blob, 12 + 8 * i)
                assert cd + 8 <= total
                magic, length = struct.unpack_from('>II', blob, cd)
                if magic != 0xfade0c02:
                    continue
                assert 44 <= length <= total - cd
                typ = blob[cd + 37]
                algo = {1: 'sha1', 2: 'sha256', 3: 'sha256', 4: 'sha384'}[typ]
                out.add((hashlib.new(algo, blob[cd:cd + length]).digest()[:20], typ))
            assert out, 'unsigned ARM64 Mach-O'
            return out
        at += length
    raise ValueError('unsigned ARM64 Mach-O')


def archive_members(path):
    # macOS bsdtar supports zstd and lzma; tarfile only receives an uncompressed stream.
    raw = subprocess.check_output(['tar', '-cf', '-', '@' + str(path)])
    return tarfile.open(fileobj=io.BytesIO(raw)), raw


def safe_name(name):
    while name.startswith('./'):
        name = name[2:]
    if name in ('', '.', 'var', 'var/'):
        return None
    assert (name == 'var/jb' or name.startswith('var/jb/')) and '..' not in name.split('/'), name
    return name


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--assets', type=Path, required=True)
    p.add_argument('--gpu-addon', type=Path, required=True)
    p.add_argument('--archive-include', type=Path, required=True,
                   help='directory holding official libarchive public headers')
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--reuse-environment', type=Path,
                   help='retain an already extracted environment and its exact revision; change only the boot entry')
    a = p.parse_args()
    out = a.output.resolve(); out.mkdir(parents=True, exist_ok=True)
    helper = out / 'coachz-apps'
    sdk = subprocess.check_output(['xcrun', '--sdk', 'iphoneos', '--show-sdk-path'], text=True).strip()
    if a.reuse_environment:
        if a.reuse_environment.resolve() == out:
            raise ValueError('reuse source and output must be separate directories')
        shutil.copyfile(a.reuse_environment / 'coachz-apps', helper)
        helper.chmod(0o755)
    else:
        subprocess.run(['xcrun', '--sdk', 'iphoneos', 'clang', '-target', 'arm64-apple-ios17.0',
                        '-isysroot', sdk, '-fobjc-arc', '-fblocks', '-O2', '-Wno-deprecated-declarations',
                        '-I', str(a.archive_include), str(ROOT / 'platform/coachz-prelink/apps/coachz-apps.m'),
                        '-framework', 'Foundation', '-larchive', '-o', str(helper)], check=True)
        subprocess.run(['codesign', '--force', '--sign', '-', '--entitlements',
                        str(ROOT / 'platform/coachz-prelink/apps/entitlements.plist'), str(helper)], check=True)
    stock = (a.assets / 'launchd-stock.plist').read_bytes()
    manifest, job = launchd_manifest(stock)
    (out / 'launchd.plist').write_bytes(manifest)
    original_daemon = (a.assets / 'mtmergeprops').read_bytes()
    original_identity = subprocess.run(['codesign', '--display', '--verbose=4', str(a.assets / 'mtmergeprops')],
                                       capture_output=True, text=True, check=True).stderr
    assert 'Identifier=com.apple.mtmergeprops\n' in original_identity, 'unexpected original daemon'
    original_check = (a.assets / 'sysstatuscheck').read_bytes()


    if a.reuse_environment:
        payload = out / 'environment.tar.gz'
        shutil.copyfile(a.reuse_environment / 'environment.tar.gz', payload)
        expected = (a.reuse_environment / 'revision').read_text()
        actual = hashlib.sha256(payload.read_bytes() + helper.read_bytes()).hexdigest() + '\n'
        assert expected == actual, 'existing environment revision does not match its files'
    else:
        # Preserve tar metadata, symlinks and package database entries without writing through links on the host.
        members = {}
        bootstrap, _ = archive_members(a.assets / 'rootless/bootstrap.tar.zst')
        for info in bootstrap:
            name = safe_name(info.name)
            if name is None:
                continue
            assert info.isfile() or info.isdir() or info.issym() or info.islnk(), name
            info.name = name
            members[name] = (info, bootstrap.extractfile(info).read() if info.isfile() else None)
        status_name = 'var/jb/Library/dpkg/status'
        status = members[status_name][1].decode()
        for filename in ['rootless/libplist3.deb', 'rootless/ldid.deb', 'sileo-rootless.deb', 'trollstorelite-rootless.deb']:
            deb = a.assets / filename
            entries = subprocess.check_output(['ar', '-t', str(deb)], text=True).splitlines()
            sections = {}
            for kind in ['control', 'data']:
                entry = next(x for x in entries if x.startswith(kind + '.tar'))
                f = out / (kind + '.tar.packed')
                f.write_bytes(subprocess.check_output(['ar', '-p', str(deb), entry]))
                sections[kind], _ = archive_members(f)
            control = sections['control'].extractfile('./control').read().decode()
            package = next(x[9:] for x in control.splitlines() if x.startswith('Package: '))
            old = [x for x in status.split('\n\n') if not x.startswith('Package: ' + package + '\n') and x.strip()]
            status = '\n\n'.join(old) + '\n\n' + control.rstrip() + '\nStatus: install ok unpacked\n\n'
            for info in sections['data']:
                name = safe_name(info.name)
                if name is None:
                    continue
                assert info.isfile() or info.isdir() or info.issym() or info.islnk(), name
                info.name = name
                members[name] = (info, sections['data'].extractfile(info).read() if info.isfile() else None)
            for info in sections['control']:
                name = info.name.removeprefix('./')
                if name in ('control', '.', '') or not info.isfile():
                    continue
                info.name = f'var/jb/Library/dpkg/info/{package}.{name}'
                members[info.name] = (info, sections['control'].extractfile(info).read())
            filelist = '\n'.join('/' + x.name.removeprefix('./').rstrip('/') for x in sections['data'].getmembers()
                                 if x.name.removeprefix('./') not in ('', '.')) + '\n'
            info = tarfile.TarInfo(f'var/jb/Library/dpkg/info/{package}.list'); info.mode = 0o644
            members[info.name] = (info, filelist.encode())

        def put(name, data, mode=0o644):
            info = tarfile.TarInfo(name); info.mode = mode; info.size = len(data)
            members[name] = info, data

        put(status_name, status.encode())
        # No default-password sshd is started. The environment is provisioned locally.
        for name in list(members):
            if name.endswith('/com.openssh.sshd.plist') or name.endswith('/us.diatr.shshd.plist'):
                del members[name]
        # Preserve upstream tools; wrappers load exact CodeDirectories, including after ldid re-signs.
        for tool in ('dpkg', 'ldid'):
            name = f'var/jb/usr/bin/{tool}'
            info, data = members[name]; info.name = name + '.coachz-real'
            members[info.name] = info, data
            put(name, helper.read_bytes(), 0o755)

        payload = out / 'environment.tar.gz'
        with payload.open('wb') as raw, gzip.GzipFile(fileobj=raw, mode='wb', mtime=0) as zipped, tarfile.open(fileobj=zipped, mode='w|') as tar:
            # Directories first, regular files next, links last: don't traverse archive-created symlinks.
            for info, data in sorted(members.values(), key=lambda v: (0 if v[0].isdir() else 2 if v[0].issym() or v[0].islnk() else 1, v[0].name)):
                info.uid = info.gid = 0; info.uname = info.gname = ''; info.mtime = 0
                if data is not None:
                    info.size = len(data)
                tar.addfile(info, io.BytesIO(data) if data is not None else None)

    hashes = cdhashes(helper.read_bytes()) | cdhashes(original_check) | cdhashes(original_daemon)
    with tarfile.open(payload) as environment:
        for info in environment:
            if info.isfile():
                hashes |= cdhashes(environment.extractfile(info).read())
    assert 0 < len(hashes) <= 32768
    tc_entries = b''.join(cdhash + bytes((typ, 0, 0, 0)) for cdhash, typ in sorted(hashes))
    (out / 'trustcache.bin').write_bytes(struct.pack('<I16sI', 2, hashlib.sha256(tc_entries).digest()[:16], len(hashes)) + tc_entries)
    revision = hashlib.sha256(payload.read_bytes() + helper.read_bytes()).hexdigest() + '\n'
    (out / 'revision').write_text(revision)
    files = [('owner-apps/coachz-apps', 0o755, helper.read_bytes()),
             ('owner-apps/environment.tar.gz', 0o644, payload.read_bytes()),
             ('owner-apps/revision', 0o644, revision.encode()),
             ('owner-apps/boot/sysstatuscheck', 0o755, original_check),
             ('owner-apps/boot/mtmergeprops', 0o755, original_daemon),
             ('owner-apps/LaunchDaemons/com.coachz.apps.plist', 0o644, job),
             ('owner-apps/xpc/launchd.plist', 0o644, manifest),
             ('owner-apps/xpc/coachz-apps-manifest', 0o644, b'direct launchd job\n')]
    gpu = a.gpu_addon.read_bytes()
    magic, count, total = struct.unpack_from('<8sI4xQ', gpu)
    assert magic == b'NGPUADD2' and total == len(gpu) and count + len(files) <= 4096
    # Preserve all existing addon entries, including their modes and compressed bytes.
    result = bytearray(gpu)
    for name, mode, data in files:
        name = name.encode() + b'\0'; packed = zlib.compress(data, 9)
        result += struct.pack('<IIQQ', len(name), mode, len(data), len(packed))
        result += name + bytes((-len(name)) % 8)
        result += packed + bytes((-len(packed)) % 8)
    struct.pack_into('<I', result, 8, count + len(files)); struct.pack_into('<Q', result, 16, len(result))
    assert result[24:len(gpu)] == gpu[24:]
    (out / 'NativeAdrenoMetal.addon').write_bytes(result)
    # Reopen the delivered addon, not just the build inputs, and check its app entries.
    delivered = (out / 'NativeAdrenoMetal.addon').read_bytes()
    entries = {}
    at = 24
    for _ in range(count + len(files)):
        name_size, mode, size, packed_size = struct.unpack_from('<IIQQ', delivered, at)
        at += 24
        name = delivered[at:at + name_size].rstrip(b'\0').decode()
        at += (name_size + 7) & ~7
        data = zlib.decompress(delivered[at:at + packed_size])
        assert len(data) == size and name not in entries, 'invalid or duplicate addon entry'
        entries[name] = mode, data
        at += (packed_size + 7) & ~7
    assert at == len(delivered), 'addon has trailing or missing data'
    for name, mode, data in files:
        assert entries[name] == (mode, data), 'delivered app entry differs: ' + name
    assert entries['owner-apps/boot/sysstatuscheck'][1] == original_check, 'old boot-task wrapper not restored'
    assert entries['owner-apps/boot/mtmergeprops'][1] == original_daemon, 'old daemon wrapper not restored'
    assert entries['owner-apps/xpc/launchd.plist'][1] == manifest, 'launchd manifest differs'
    tc = (out / 'trustcache.bin').read_bytes()
    trusted = {(tc[i:i + 20], tc[i + 20]) for i in range(24, len(tc), 24)}
    for name in ('owner-apps/boot/sysstatuscheck', 'owner-apps/boot/mtmergeprops', 'owner-apps/coachz-apps'):
        assert cdhashes(entries[name][1]) <= trusted, 'delivered executable missing from trust cache'
    evidence = {'gpu_addon_sha256': hashlib.sha256(gpu).hexdigest(), 'addon_sha256': hashlib.sha256(result).hexdigest(),
                'archive_sha256': hashlib.sha256(payload.read_bytes()).hexdigest(), 'trusted_code_directories': len(hashes),
                'bootstrap_bytes': payload.stat().st_size, 'addon_bytes': len(result), 'runtime_accepted': False,
                'environment_reused': bool(a.reuse_environment), 'revision': revision.strip(),
                'bootstrap_method': 'direct job in launchd boot manifest',
                'boot_entry_target': BOOT_ENTRY_TARGET,
                'launchd_manifest_sha256': hashlib.sha256(manifest).hexdigest(),
                'original_launchd_manifest_sha256': hashlib.sha256(stock).hexdigest(),
                'original_jobs_preserved': len(plistlib.loads(stock)['LaunchDaemons']),
                'installer_job': 'com.coachz.apps',
                'sandbox_declaration': 'com.apple.private.security.no-sandbox',
                'original_daemon_sha256': hashlib.sha256(original_daemon).hexdigest(),
                'sysstatuscheck_restored': True, 'mtmergeprops_restored': True,
                'requires_kernel_rebuild': True}
    (out / 'manifest.json').write_text(json.dumps(evidence, indent=2) + '\n')
    print(json.dumps(evidence, indent=2))


if __name__ == '__main__':
    main()
