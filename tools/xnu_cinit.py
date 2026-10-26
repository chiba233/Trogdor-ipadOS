"""Source-specific audit for the bounded native arm_init C checkpoint."""
import hashlib
import struct

from xnu_checkpoint import symbols
from xnu_macho import backed

CHECKPOINT_OFFSET = 0x594
METADATA = struct.Struct('<6Q')
KERNEL_SHA256 = '6b406b8cd4ee5a6c11793256d8cadc5453365e8e8edb5f487953e02e259c7e9b'
# This audit covers the standalone-header, empty-thread_starts branch only.
AUDITED = {
    '_arm_init': (0x598, '8c5ff5665073096c490d9522896124874d1b9677f605757b8972b9d801c2f287'),
    '_arm_set_kernel_tbi': (4, '110f46b5b35c069160560c6ad6786f647dd44e8760a52a46fc22dbbcd7630b91'),
    '_memmove': (0x1ac, '9c27385f291628c7f64a0b3f90c57ae8bdbe39a73e692c8fb6afe238ffefd0fb'),
}
NAMES = ('_arm_init', '_const_boot_args', '_BootArgs', '_vm_kernel_slide',
         '_static_kernelcache', '___stack_chk_guard')


def audit_metadata(data, kernel):
    if hashlib.sha256(data).hexdigest() != KERNEL_SHA256:
        raise ValueError('C initialization requires the exact independently audited source kernel')
    names = symbols(data, (*NAMES, *AUDITED))
    for name, (size, digest) in AUDITED.items():
        at = backed(kernel.segments, names[name], size)
        if hashlib.sha256(data[at:at+size]).hexdigest() != digest:
            raise ValueError('C initialization instruction audit changed: ' + name)
    return METADATA.pack(*(names[key] for key in NAMES))
