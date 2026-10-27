"""Pinned full pre-VM PE return audit and an independent bounded ADT contract."""
import hashlib
import struct
from xnu_checkpoint import symbols, adt_fields
from xnu_cinit import KERNEL_SHA256
from xnu_macho import backed

COMMAND_LINE = '-v debug=0xc cpus=1 keepsyms=1'
NAMES = ('_gPESoCDeviceType', '_iBoot_version', '_gPEKernelConfigurationBitmask',
         '_debug_enabled', '_debug_cpu_performance_degradation_factor')
METADATA = struct.Struct('<5Q')
AUDITED = {'_pe_arm_get_soc_base_phys': (200, '0331c0c3ba7f3d2f8721d419ec1360f13cbf689ac26a0bf70046167f0e9c17dd'),
 '_pe_identify_machine': (860, '51d04cc9a4b4de5ad77be511fc5cf44fbfac9959e69e21dd39daf9efc89051b2'),
 '_PE_init_platform': (908, 'e81ec6058c4c2e76bb6014aa0db5e4809ececab3585b3fc7856da3f0895f15ce'),
 '_SecureDTLookupEntry': (512, '4c4fa92fbee869ac5922393e227a11c56e7e1daac97c9eb20e651ac397dc839b'),
 '_SecureDTInitEntryIterator': (56, 'a0a0886dccd39e58600011191db1ed42c1fbb09a7ed6eb090295baa5d6e9cecb'),
 '_SecureDTIterateEntries': (152, '497d6db1bd2d30d55f3953fbd4b933e1205bf2bf1543f5954f040cfea854e357'),
 '_SecureDTGetProperty': (24, '615530461b71a596335f2f5c7622beb9d22e58610f95830a67e1d0e49a27b5fa'),
 '_SecureDTGetPropertyInternal': (364, '375d3ebe3eb8c12a964ecc2b11bbc45ecd518b5d76592c312d3d1fe537a22bdb'),
 '_skipTree': (176, 'd8aa47c815b7170c0e4ca8a99967455f7f3d0b143c305e5ca4dc9928909b69fa'),
 '_skipProperties': (164, '7262ec956f85414e901a1b79dc9a2ca874368baad739247b8ff00f3f2deb6be1'),
 '_pe_init_debug': (396, 'e177489f52b9b5da28ba187e4e8f0585ae65b4b9cef863d3de06ea7888a905be'),
 '_PE_boot_args': (20, 'b65320e74c8be6889f34567b4c56554f5cbb865aab86c23dea9a11b063cb7927'),
 '_PE_parse_boot_argn': (64, '4efe0a9a016b6f03701a5ed8c1531943d3a5580aca3bd2bdd6284987b4a96652'),
 '_PE_parse_boot_argn_internal': (1440, 'b91be3b0364a94d7bcb2af3ef3426dcf37dbae995d6bf1c6fe35a6dad9f325d6'),
 '_PE_i_can_has_debugger': (68, 'c2213e475f0a95e91f5f5c60413ecda2441199698dff2089ece078ad1b32cf2b'),
 '_strncmp': (396, '3863175bf242b4d1a38ade1e660d6b2a2e7b4bec717227cd4a8d018b227ff78f')}


def tree_contract(data):
    adt_fields(data)
    if len(data)>65536: raise ValueError('PE ADT size limit')
    at=0;nodes=0;seen=set();out=[0]*8
    allowed={
        'root': {'name','compatible','#address-cells','#size-cells','model'},
        'chosen': {'name','random-seed','dram-base','dram-size','nvram-bank-size',
                   'nvram-bank-count','nvram-current-bank','nvram-proxy-data','osenvironment'},
        'cpus': {'name'},
        'cpu0': {'name','reg','state','device_type','compatible','timebase-frequency'},
        'arm-io': {'name','device_type','#address-cells','#size-cells','ranges'}}
    def string(value,limit):
        return 1<=len(value)<=limit and value[-1:]==b'\0' and b'\0' not in value[:-1] and all(0<q<128 for q in value[:-1])
    def visit(depth,parent):
        nonlocal at,nodes
        if depth>4 or nodes>=64 or at+8>len(data):raise ValueError('PE ADT node/depth limit')
        nodes+=1;count,children=struct.unpack_from('<II',data,at);at+=8
        if not 1<=count<=32 or children>64:raise ValueError('PE ADT property/child limit')
        props={}
        for i in range(count):
            if at+36>len(data):raise ValueError('PE ADT property bounds')
            raw=data[at:at+32];key=raw.split(b'\0')[0]
            if not 1<=len(key)<=31 or raw!=key.ljust(32,b'\0') or any(q>=128 for q in key):raise ValueError('PE ADT canonical key')
            key=key.decode();size=struct.unpack_from('<I',data,at+32)[0];value=at+36;end=(value+size+3)&~3
            if end>len(data) or any(data[value+size:end]) or key in props:raise ValueError('PE ADT property bounds/padding')
            if i==0 and key!='name':raise ValueError('PE ADT name must be first')
            props[key]=(value,data[value:value+size]);at=end
        name=props['name'][1]
        if not string(name,32):raise ValueError('PE ADT node name')
        name=name[:-1].decode()
        if depth==0:
            if name!='device-tree':raise ValueError('PE ADT root')
            role='root'
        elif depth==1:
            if name not in ('chosen','cpus','arm-io','defaults','options','product') or name in seen:raise ValueError('PE ADT root children')
            seen.add(name);role=name
        elif parent=='cpus':
            if name!='cpu0' or children:raise ValueError('PE ADT unique CPU')
            role='cpu0'
        else:
            if name in ('device-tree','chosen','cpus','cpu0','arm-io'):raise ValueError('PE ADT ambiguous node')
            role='other'
        if role in allowed and not props.keys()<=allowed[role]:raise ValueError('PE ADT unaudited property')
        if role=='root':
            off,val=props['model']
            if not string(val,32):raise ValueError('PE ADT model string')
            out[0:2]=off,len(val)
        elif role=='chosen':
            for i,key in enumerate(('dram-base','dram-size')):
                off,val=props[key]
                if len(val)!=8:raise ValueError('PE ADT DRAM size')
                out[6+i]=off
        elif role=='cpus' and children!=1:raise ValueError('PE ADT unique CPU')
        elif role=='cpu0':
            if props['state'][1]!=b'running\0' or len(props['timebase-frequency'][1])!=4:raise ValueError('PE ADT CPU state/frequency')
            out[5]=int.from_bytes(props['timebase-frequency'][1],'little')
        elif role=='arm-io':
            off,val=props['device_type']
            if not string(val,32):raise ValueError('PE ADT SoC type string')
            out[2:4]=off,len(val)
            raw=props['ranges'][1]
            if len(raw)!=24 or not int.from_bytes(raw[8:16],'little'):raise ValueError('PE ADT nonzero SoC parent')
            out[4]=int.from_bytes(raw[8:16],'little')
        for _ in range(children):visit(depth+1,role)
    try:visit(0,'')
    except (KeyError,UnicodeError,struct.error) as exc:raise ValueError('PE ADT missing/malformed property') from exc
    if at!=len(data) or not {'chosen','cpus','arm-io'}<=seen or not all(out):raise ValueError('PE ADT incomplete')
    return tuple(out)


def audit_metadata(data,kernel,adt,command_line):
    if hashlib.sha256(data).hexdigest()!=KERNEL_SHA256:raise ValueError('PE return requires exact audited source kernel')
    if command_line!=COMMAND_LINE:raise ValueError('PE return requires fixed audited command line')
    tree_contract(adt)
    extra=('_gPESoCDeviceTypeBuffer','_gPESoCBasePhys','_gModelTypeBuffer','_gPEClockFrequencyInfo','_arm_init')
    names=symbols(data,(*NAMES,*AUDITED,*extra))
    for name,(size,digest) in AUDITED.items():
        at=backed(kernel.segments,names[name],size)
        if hashlib.sha256(data[at:at+size]).hexdigest()!=digest:raise ValueError('PE instruction audit changed: '+name)
    if (names['_gPESoCDeviceTypeBuffer']!=names[NAMES[0]]+8 or names['_gPESoCBasePhys']!=names[NAMES[0]]+40 or
        names['_gModelTypeBuffer']!=names[NAMES[1]]+0x110 or names['_gPEClockFrequencyInfo']!=names[NAMES[1]]+0x140):
        raise ValueError('unsupported PE globals layout')
    return METADATA.pack(*(names[key] for key in NAMES)),names['_arm_init']+0x5b0
