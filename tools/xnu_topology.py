"""Pinned WFE/CPU-topology prefix and independent input metadata."""
import hashlib
import struct
from xnu_cinit import KERNEL_SHA256
from xnu_checkpoint import symbols
from xnu_macho import backed
from xnu_pe import tree_contract

NAMES=('_topology_info','_topology_cpu_array','_topology_cluster_array','_cluster_offsets',
       '_cpu_config_correct','_arm64_eventi','_PAGE_SHIFT_CONST','_page_shift_user32','_xnu_platform_stall_value')
SIZES=(88,480,72,8,1,4,4,4,8)
METADATA=struct.Struct('<10Q')
AUDITED={'_arm_init': (1612, 'def57c80f6333c5ee10853657bd80439fd775cc3474e24787a342a6e56b88324'), '_configure_timer_apple_regs': (4, '110f46b5b35c069160560c6ad6786f647dd44e8760a52a46fc22dbbcd7630b91'), '_configure_misc_apple_boot_args': (4, '110f46b5b35c069160560c6ad6786f647dd44e8760a52a46fc22dbbcd7630b91'), '_configure_misc_apple_regs': (4, '110f46b5b35c069160560c6ad6786f647dd44e8760a52a46fc22dbbcd7630b91'), '_wfe_timeout_configure': (212, 'e03e8899a83be410eec308b1a7feb4a372411d62eb9ab0865a225390deaaa7dc'), '_wfe_timeout_init': (88, 'bb27ee156d00197bf0ea636d2d98945e12c03015dc3e4dbc0f8dea38a340f0fc'), '_flsll': (24, '346100519325c6234da3dd359716357155e9f9b0c807d6269822432fef1dbc1b'), '_platform_stall_panic_or_spin': (76, '3e09c0b5bbcf4ba5ff5a518b75876a975eb1dc304cbb5c1526f42dc29395f99a'), '_ml_parse_cpu_topology': (2240, 'dcc4112534fba2ce68fb078a600cc94d4abe4689ff0d916c7494abb357d33972'), '_ml_readprop': (236, '372608dba828d2ad770293c97aa667b4b669fa7dedc790a88fbb7136962bef1e')}


def cpu_reg_offset(data):
    tree_contract(data)
    at=0;found=[]
    def visit(depth,parent):
        nonlocal at
        count,children=struct.unpack_from('<II',data,at);at+=8;props={}
        for _ in range(count):
            key=data[at:at+32].split(b'\0')[0].decode();size=struct.unpack_from('<I',data,at+32)[0]
            props[key]=(at+36,size);at=(at+36+size+3)&~3
        off,size=props['name'];name=data[off:off+size-1].decode()
        if depth==2 and parent=='cpus':found.append(props['reg'][0])
        for _ in range(children):visit(depth+1,name)
    visit(0,'')
    if len(found)!=1:raise ValueError('one CPU reg required')
    return found[0]


def initial_bytes(index,values):
    out=bytearray(SIZES[index])
    if index==0:
        struct.pack_into('<I',out,0,1);struct.pack_into('<2Q',out,24,values[1],values[2])
    elif index==4:out[0]=1
    elif index==5:out[:4]=b'\xff'*4
    return bytes(out)


def audit_metadata(data,kernel,adt):
    if hashlib.sha256(data).hexdigest()!=KERNEL_SHA256:raise ValueError('topology requires pinned source kernel')
    names=symbols(data,(*NAMES,*AUDITED))
    for name,(size,digest) in AUDITED.items():
        off=backed(kernel.segments,names[name],size)
        if hashlib.sha256(data[off:off+size]).hexdigest()!=digest:raise ValueError('topology instruction audit changed: '+name)
    values=[names[key] for key in NAMES]
    for index,(va,size) in enumerate(zip(values,SIZES)):
        contents=None
        for start,extent,off,backing,prot in kernel.segments:
            if start<=va and va+size<=start+extent and prot&1 and not prot&4:
                rel=va-start;contents=(data[off+rel:off+min(rel+size,backing)] if rel<backing else b'').ljust(size,b'\0')
        if contents!=initial_bytes(index,values):raise ValueError('topology initializer changed: '+NAMES[index])
    reg=cpu_reg_offset(adt)
    stop=names['_arm_init']+0x648
    if struct.unpack_from('<I',data,backed(kernel.segments,stop,4))[0]!=0x97e9037c:raise ValueError('topology stop changed')
    return METADATA.pack(*values,reg),stop
