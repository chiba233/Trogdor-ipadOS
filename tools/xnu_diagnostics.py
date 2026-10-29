"""Pinned synchronous-vector instrumentation for the bounded SoC diagnostic."""
import hashlib
import struct
from xnu_cinit import KERNEL_SHA256
from xnu_macho import backed

FAULT_HVC=0xd4000002|(0x5846<<5)


def instrument_vectors(original,patched,kernel,vbar):
    if hashlib.sha256(original).hexdigest()!=KERNEL_SHA256 or vbar&2047:
        raise ValueError('diagnostics require the exact audited kernel/vector')
    patches=[]
    for offset,word in ((0,0x14000400),(0x200,0x140003fb)):
        va=vbar+offset;at=backed(kernel.segments,va,4)
        if struct.unpack_from('<I',original,at)[0]!=word:
            raise ValueError('unaudited synchronous vector instruction')
        if not any(start<=va and va+4<=start+size and prot&4 for start,size,_,_,prot in kernel.segments):
            raise ValueError('synchronous vector is not executable')
        struct.pack_into('<I',patched,at,FAULT_HVC)
        patches.append({'file_offset':at,'va':va,'bytes':4})
    return patches


def instrument_control(original,patched,kernel,va):
    at=backed(kernel.segments,va,4)
    if struct.unpack_from('<I',original,at)[0]!=0xaa0003e8:
        raise ValueError('unaudited fault-control boundary')
    struct.pack_into('<I',patched,at,0xd4200000|(0x5846<<5))
    return {'file_offset':at,'va':va,'bytes':4,'deliberate_fault':True}
