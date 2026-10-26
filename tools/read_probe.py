#!/usr/bin/env python3
"""Read and validate the native probe text exported through Linux's device tree."""
import argparse
from pathlib import Path
import re

DEFAULT = Path('/sys/firmware/devicetree/base/chosen/trogdor-ipados,probe-log')


def read_record(data):
    if len(data) != 8192 or b'\0' not in data:
        raise ValueError('invalid probe log capacity or missing terminator')
    text, rest = data.split(b'\0', 1)
    if any(rest) or not text.startswith(b'Trogdor-ipadOS native phase-0 probe\n'):
        raise ValueError('empty, stale, or unrecognized probe log')
    text = text.decode('ascii')
    fields = {}
    for key in ('entry EL', 'current EL', 'stage', 'handoff EL'):
        values = re.findall(r'^' + re.escape(key) + r'=(0x[0-9a-f]{16})$', text, re.M)
        if len(values) != 1:
            raise ValueError('missing/duplicate probe field: ' + key)
        fields[key] = int(values[0], 16)
    if fields != {'entry EL': 2, 'current EL': 1, 'stage': 3, 'handoff EL': 2}:
        raise ValueError('probe did not complete the required EL2 -> EL1 -> EL2 path')
    return text, fields


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--input', type=Path, default=DEFAULT)
    args = parser.parse_args()
    with args.input.open('rb') as source:
        data = source.read(8193)
    text, _ = read_record(data)
    print(text, end='')


if __name__ == '__main__':
    main()
