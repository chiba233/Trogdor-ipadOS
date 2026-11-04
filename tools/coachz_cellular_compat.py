#!/usr/bin/env python3
"""Prepare the pinned CommCenter for Coachz cellular and optional eSIM compatibility."""
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
ESIM_GUARD_ADDRESS = 0x10000e3ec
ESIM_BRANCH_ADDRESS = 0x10000e410
ESIM_ORIGINAL_GUARD = bytes.fromhex(
    'ffc301d1f44f05a9fd7b06a9fd830191f30300aa00a040f9080040f9082140f900013fd6'
    'a0040036e00313aa2f20009400060036609a40f9080040f908a140f96100805200013fd60007003660a240f9')
ESIM_BRANCH = struct.pack('<I', 0xd503201f)  # NOP: continue to modification, Vinyl and setup-state checks.
ESIM_FLOW_GUARD_ADDRESS = 0x10000e5ec
ESIM_FLOW_ADDRESS = 0x10000e61c
ESIM_FLOW_GUARD = bytes.fromhex(
    '60a240f9080040f9081540f900013fd61f00007194129f1a60a240f9080040f9081940f9'
    '00013fd688021f321f0000710011941acfffff17')
ESIM_FLOW = struct.pack('<I', 0x2a0803e0)  # MOV W0, W8: include the GSMA flow bit (0x2) for the real Tape card.
ESIM_INSTALL_GUARD_ADDRESS = 0x10027dba4
ESIM_INSTALL_ADDRESS = 0x10027dbbc
ESIM_INSTALL_GUARD = bytes.fromhex(
    '68624139c800003580a240f9080040f9081940f900013fd600120034ffcb00f9751a40f9'
    'e0430691ff26f797351100b4')
ESIM_INSTALL = struct.pack('<I', 0xd503201f)  # NOP: the Tape card supplies GSMA independently of the legacy default card type.
ESIM_ELIGIBILITY_GUARD_ADDRESS = 0x1009aaa00
ESIM_ELIGIBILITY_ADDRESS = 0x1009aaa08
ESIM_ELIGIBILITY_GUARD = bytes.fromhex(
    '16008052e8034139a8090034e883403968090034a1a20091a0420091a3020191e2a30091'
    'e46302911811ff97000b0036')
# CBZ W8, 0x1009aa9c0: only absent local Apple signature material takes the native soft-failure path.
# The following URL/encoding checks and the remote eligibility response handler retain their original failures.
ESIM_ELIGIBILITY = struct.pack('<I', 0x34fffdc8)


def guard_offset(image, address=GUARD_ADDRESS, length=len(ORIGINAL_GUARD)):
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
            segment_address, _, file_offset, file_size = struct.unpack_from('<4Q', image, at + 24)
            if segment_address <= address and address + length <= segment_address + file_size:
                offset = file_offset + address - segment_address
        at += size
    if uuid != SOURCE_UUID or offset is None:
        raise ValueError('CommCenter does not match the pinned OS build')
    return offset


def patch_esim(image):
    """The legacy radio identity lacks local signup capability despite the real Tape eUICC."""
    edits = []
    for address, guard, branch, instruction in (
            (ESIM_GUARD_ADDRESS, ESIM_ORIGINAL_GUARD, ESIM_BRANCH_ADDRESS, ESIM_BRANCH),
            (ESIM_FLOW_GUARD_ADDRESS, ESIM_FLOW_GUARD, ESIM_FLOW_ADDRESS, ESIM_FLOW),
            (ESIM_INSTALL_GUARD_ADDRESS, ESIM_INSTALL_GUARD, ESIM_INSTALL_ADDRESS, ESIM_INSTALL),
            (ESIM_ELIGIBILITY_GUARD_ADDRESS, ESIM_ELIGIBILITY_GUARD, ESIM_ELIGIBILITY_ADDRESS, ESIM_ELIGIBILITY)):
        offset = guard_offset(image, address, len(guard))
        if image[offset:offset + len(guard)] != guard:
            raise ValueError('eSIM flow instruction guard differs')
        edits.append((offset + branch - address, instruction))
    for offset, instruction in edits:
        image[offset:offset + 4] = instruction


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
    parser.add_argument('--esim', action='store_true', help='enable the native local eSIM signup capability gate for the Coachz Tape bridge')
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
    if args.esim:
        patch_esim(patched)
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
