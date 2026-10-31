#!/usr/bin/env python3
"""Strict acceptance of the read-only USB experiment: linux_log.py --usb-read."""
import re

from read_probe import read_record

USBSTS_ERRORS = (1 << 2) | (1 << 12)  # HSE, HCE


def read_usb_record(data):
    text, _ = read_record(data)
    fields = {}
    for key in ('USB A controller', 'USB A USBSTS poll', 'USB A disk', 'USB A speed', 'USB A blocks', 'USB A block size',
                'USB A lba0 rc', 'USB A lba0 tail', 'USB A lba1 rc', 'USB A lba1 head', 'USB A lba2048 rc',
                'USB A lba2048 head', 'USB A SMMU sGFSR', 'USB SMMU sGFSR', 'USB fatal count', 'USB done'):
        values = re.findall(r'^' + re.escape(key) + r'=(0x[0-9a-f]{16})$', text, re.M)
        if len(values) != 1:
            raise ValueError('missing/duplicate USB field: ' + key)
        fields[key] = int(values[0], 16)
    expected = {'USB A controller': 1, 'USB A disk': 1, 'USB A block size': 512, 'USB A lba0 rc': 0, 'USB A lba1 rc': 0,
                'USB A lba2048 rc': 0, 'USB A SMMU sGFSR': 0, 'USB SMMU sGFSR': 0, 'USB fatal count': 0, 'USB done': 1,
                'USB A lba1 head': int.from_bytes(b'EFI PART', 'little'),
                'USB A lba2048 head': int.from_bytes(b'CHROMEOS', 'little')}
    for key, value in expected.items():
        if fields[key] != value:
            raise ValueError('USB field differs: ' + key)
    if fields['USB A USBSTS poll'] & USBSTS_ERRORS or fields['USB A lba0 tail'] >> 48 != 0xaa55 or not fields['USB A blocks']:
        raise ValueError('USB controller error, bad MBR signature or empty medium')
    return text, fields
