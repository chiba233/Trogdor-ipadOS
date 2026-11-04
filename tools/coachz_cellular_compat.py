#!/usr/bin/env python3
"""Prepare the pinned CommCenter for a modem without Apple postponement tickets."""
import argparse
import hashlib
from pathlib import Path
import struct
import subprocess
import tempfile
import zlib

from xnu_init import cdhash

SOURCE_SHA256 = '383779eda4bbad539f8809d39b109ffd5a003335824c170f0a81cd8073e519b1'
SOURCE_UUID = bytes.fromhex('f1d178cf7f4c3576b9d70dc14ac6f82d')
GUARD_ADDRESS = 0x100fa5a54
BRANCH_ADDRESS = 0x100fa5a78
ORIGINAL_GUARD = bytes.fromhex(
    'f44fbea9fd7b01a9fd430091f40301aaf30300aa007440f9080040f908c540f900013fd6'
    '40010037607640f9080040f908dd40f900013fd6a000003628008012880200b9200080520c00001464530894')
COMPAT_BRANCH = struct.pack('<I', 0x1400000a)  # B 0x100fa5aa0: continue normal context selection.


def guard_offset(image):
    """Resolve the instruction through LC_SEGMENT_64 and require the pinned LC_UUID."""
    if len(image) < 32 or struct.unpack_from('<II', image) != (0xfeedfacf, 0x100000c):
        raise ValueError('expected a thin arm64 Mach-O')
    at, uuid, offset = 32, None, None
    for _ in range(struct.unpack_from('<I', image, 16)[0]):
        command, size = struct.unpack_from('<II', image, at)
        if size < 8 or at + size > len(image):
            raise ValueError('invalid Mach-O load command')
        if command == 0x1b:
            uuid = image[at + 8:at + 24]
        if command == 0x19:
            address, _, file_offset, file_size = struct.unpack_from('<4Q', image, at + 24)
            if address <= GUARD_ADDRESS and GUARD_ADDRESS + len(ORIGINAL_GUARD) <= address + file_size:
                offset = file_offset + GUARD_ADDRESS - address
        at += size
    if uuid != SOURCE_UUID or offset is None:
        raise ValueError('CommCenter does not match the pinned OS build')
    return offset


def validate_compat(image):
    offset = guard_offset(image)
    expected = bytearray(ORIGINAL_GUARD)
    branch = BRANCH_ADDRESS - GUARD_ADDRESS
    expected[branch:branch + 4] = COMPAT_BRANCH
    if image[offset:offset + len(expected)] != expected:
        raise ValueError('CommCenter compatibility instructions do not match')


def compat_trust_cache(image):
    """One SHA-256 entry, in the same v2 module format as the GPU and audio tools."""
    validate_compat(image)
    digest = cdhash(image)
    identity = hashlib.sha256(b'Coachz cellular compatibility' + digest).digest()[:16]
    return struct.pack('<I16sI', 2, identity, 1) + digest + bytes((2, 0, 0, 0))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--commcenter', type=Path, required=True, help='original CommCenter from the pinned OS')
    parser.add_argument('--output', type=Path, required=True, help='directory for the signed copy and trust cache')
    parser.add_argument('--compact', action='store_true', help='use the optional zopfli module for a smaller zlib stream')
    args = parser.parse_args()
    original = args.commcenter.read_bytes()
    if hashlib.sha256(original).hexdigest() != SOURCE_SHA256:
        parser.error('source hash differs from the inspected CommCenter')
    offset = guard_offset(original)
    if original[offset:offset + len(ORIGINAL_GUARD)] != ORIGINAL_GUARD:
        parser.error('source instruction guard differs')
    patched = bytearray(original)
    branch = offset + BRANCH_ADDRESS - GUARD_ADDRESS
    patched[branch:branch + 4] = COMPAT_BRANCH
    args.output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='.cellular-', dir=args.output) as temporary:
        executable = Path(temporary) / 'CommCenter'
        executable.write_bytes(patched)
        executable.chmod(0o755)
        subprocess.run(['/usr/bin/codesign', '--force', '--sign', '-', '--preserve-metadata=identifier,entitlements,flags,runtime',
                        '--generate-entitlement-der', '--digest-algorithm=sha256', str(executable)], check=True)
        subprocess.run(['/usr/bin/codesign', '--verify', '--strict', str(executable)], check=True)
        signed = executable.read_bytes()
        module = compat_trust_cache(signed)
        if args.compact:
            import zopfli.zlib
            packed = zopfli.zlib.compress(signed, numiterations=1)
        else:
            packed = zlib.compress(signed, 9)
        if zlib.decompress(packed) != signed:
            raise ValueError('compressed compatibility copy does not round-trip')
        (args.output / 'CommCenter.zlib').write_bytes(packed)
        (args.output / 'trustcache.bin').write_bytes(module)
        executable.replace(args.output / 'CommCenter')
    print('Prepared CommCenter compatibility copy, sha256', hashlib.sha256(signed).hexdigest())
    print('The cellular payload must include this copy and its matching trust cache.')


if __name__ == '__main__':
    main()
