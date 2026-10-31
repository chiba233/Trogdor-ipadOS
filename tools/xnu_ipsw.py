#!/usr/bin/env python3
"""Extract the restore RAM disk and its trust cache from a local IPSW into ignored build output.

Apple files: never commit or publish the input or the output.
"""
import argparse
import hashlib
from pathlib import Path
import plistlib
import zipfile


def der(data, at):
    tag, length = data[at], data[at + 1]
    at += 2
    if length & 0x80:
        count = length & 0x7f
        length = int.from_bytes(data[at:at + count], 'big')
        at += count
    return tag, at, length


def im4p_payload(data, kind):
    """The DER IM4P sequence is (IM4P, type, version, data, ...)."""
    tag, start, length = der(data, 0)
    items, at = [], start
    while at < start + length and len(items) < 4:
        _, body, size = der(data, at)
        items.append(data[body:body + size])
        at = body + size
    if tag != 0x30 or len(items) != 4 or items[0] != b'IM4P' or items[1] != kind:
        raise ValueError('not an IM4P of type ' + kind.decode())
    return items[3]


def extract(ipsw, output, device='j120ap', behavior='Update'):
    with zipfile.ZipFile(ipsw) as archive:
        manifest = plistlib.loads(archive.read('BuildManifest.plist'))
        identity = next(i for i in manifest['BuildIdentities']
                        if i['Info']['DeviceClass'] == device and i['Info']['RestoreBehavior'] == behavior)
        files = {'RestoreRamDisk': ('ramdisk.apfs', b'rdsk'), 'RestoreTrustCache': ('trustcache.bin', b'rtsc')}
        output.mkdir(parents=True, exist_ok=True)
        result = {}
        for component, (name, kind) in files.items():
            payload = im4p_payload(archive.read(identity['Manifest'][component]['Info']['Path']), kind)
            (output / name).write_bytes(payload)
            result[name] = hashlib.sha256(payload).hexdigest()
    return result


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--ipsw', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    for name, digest in extract(a.ipsw, a.output).items():
        print(name, digest)


if __name__ == '__main__':
    main()
