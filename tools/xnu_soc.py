"""Source-audited midpoint after native SoC properties, before frame restore."""
import struct
from xnu_checkpoint import symbols
from xnu_pe import audit_metadata as pe_metadata, tree_contract
from xnu_platform import search_offsets


def ranges_offset(adt):
    tree_contract(adt)
    node,_=search_offsets(adt)
    count=struct.unpack_from('<I',adt,node)[0];at=node+8
    for _ in range(count):
        key=adt[at:at+32].split(b'\0')[0];size=struct.unpack_from('<I',adt,at+32)[0]
        value=at+36
        if key==b'ranges':
            if size!=24:raise ValueError('SoC ranges must contain three 64-bit cells')
            return value
        at=(value+size+3)&~3
    raise ValueError('SoC ranges missing')


def audit_metadata(data,kernel,adt,command_line):
    metadata,_=pe_metadata(data,kernel,adt,command_line)
    name=symbols(data,('_pe_arm_get_soc_base_phys',))['_pe_arm_get_soc_base_phys']
    return metadata+struct.pack('<Q',ranges_offset(adt)),name+0xb8
