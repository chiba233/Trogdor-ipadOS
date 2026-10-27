"""Exact source audit for the bounded native boot-CPU initializer."""
import hashlib
import struct

from xnu_checkpoint import symbols
from xnu_cinit import KERNEL_SHA256
from xnu_macho import backed

CHECKPOINT_OFFSET = 0x5a4
METADATA = struct.Struct('<2Q')
AUDITED = {
    '_arm_init': (0x5a8, '07b2bc65ec5dabcfa6411928641c8a23259f0cb3c898f86bf8b5e11270aa2e57'),
    '_cpu_data_init': (0xc4, '8dac2e650f35072cd18309212ad84cac0b7f0854371515fc5615bc665a1b65a7'),
}


def audit_metadata(data, kernel):
    if hashlib.sha256(data).hexdigest() != KERNEL_SHA256:
        raise ValueError('CPU initialization requires the exact independently audited source kernel')
    names = symbols(data, ('_arm_init',))
    for name, (size, digest) in AUDITED.items():
        at = backed(kernel.segments, names[name], size)
        if hashlib.sha256(data[at:at+size]).hexdigest() != digest:
            raise ValueError('CPU initialization instruction audit changed: ' + name)
    # BootCpuData is a source macro for this defined per-CPU symbol.
    return METADATA.pack(names['_percpu_slot_cpu_data'], names['_RTClockData'])
