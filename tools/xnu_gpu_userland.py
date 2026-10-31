#!/usr/bin/env python3
"""Pack the Metal plug-in and Turnip for coachz-gpu=9: the addon file the kernel installs on the Data volume, and the trust cache module for their code."""
# SPDX-License-Identifier: BSD-2-Clause
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[1]
UNIXNU = ROOT / 'upstream/unixnu'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT / 'build/coachz-gpu-userland')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    addon = args.output / 'NativeAdrenoMetal.addon'
    addon.unlink(missing_ok=True)
    subprocess.run(['python3', '-B', 'scripts/make-gpu-addon.py', '--publish', str(addon)], cwd=UNIXNU, check=True)
    trust = json.loads((UNIXNU / 'build/adreno-metal-plugin/trust.json').read_text())
    # A version 2 module, as build-coachz-audio.py writes: SHA-256 (hash type 2), no flags, no constraint category, sorted by hash.
    hashes = sorted(bytes.fromhex(trust[key]) for key in ('cdhash', 'turnip_cdhash', 'daemon_cdhash'))
    identity = hashlib.sha256(b'Coachz GPU trust cache').digest()[:16]
    (args.output / 'trustcache.bin').write_bytes(struct.pack('<I16sI', 2, identity, len(hashes)) + b''.join(h + bytes((2, 0, 0, 0)) for h in hashes))
    print('addon', addon.stat().st_size, 'bytes,', len(hashes), 'trusted hashes')


if __name__ == '__main__':
    main()
