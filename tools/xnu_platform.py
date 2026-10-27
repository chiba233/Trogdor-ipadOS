"""Audit the bounded original platform prefix and its first native ADT search."""
import hashlib
import struct

from xnu_checkpoint import symbols, adt_fields
from xnu_cinit import KERNEL_SHA256
from xnu_macho import backed

METADATA = struct.Struct('<9Q')
AUDITED = {'_arm_init': (1460, '12b3f3bca71f9ffe8d8b0099a089ecd6fce80699131649ab7935608c5b6135b1'),
 '_PE_init_platform': (244, '221a656eee9ca4f17143081667122179903b1d89bade68b4f2c41e8a0faaf358'),
 '_pe_identify_machine': (36, 'ee924930858cd969d402b566f607033a1c5cc023aa7fc54b50685bb8a0431364'),
 '_pe_arm_get_soc_base_phys': (56, '109a953c7b75a722bf5f4ab7ee349dc72b2c2ed6161febf10a980f3e8d5d2b53'),
 '_SecureDTInit': (56, 'b3fb1bda5eb4a0ed2c3f2f08dc2f3a303bbbeb7efd7d9f887a10a1ad1bfa2a90'),
 '_SecureDTFindEntry': (40, '551f1cc5b19d2a344c14be524f685b0325eb3a8f1c8f03bb097844ad1f42ccc4'),
 '_find_entry': (404, 'b63048ada14f89820402c3167da83fb8be3f2db62c939a8ccee44e30d4a30367'),
 '_strlcpy': (116, '11f4127ecde7096c742ba8ca2a8ce9d5929f2e4852e5f23b8faba13112c100f9'),
 '_strcmp': (44, '86ff66525c3297b201ba0635f03c57bd36f2b4f5a564760460783b65f525aa95'),
 '_strlen': (100, 'ef63143ce2ac683c2b34a6bfaab86b36626c42735c99c7706a937203b96cc628'),
 '_ml_page_protection_type': (8, '2f6110664dbd16b514a0c2c7c8326932a36e85d2f15b91e57ca76efd2c7a625a')}
NAMES = ('_PE_state','_page_protection_type','_DTRootNode','_startingP',
         '_PE_init_platform','_pe_identify_machine','_pe_arm_get_soc_base_phys')


def search_offsets(data):
    adt_fields(data)
    if len(data)>65536:raise ValueError('platform ADT exceeds bounded search size')
    at=0;nodes=0;found=[]
    def visit(depth):
        nonlocal at,nodes
        if depth>4 or nodes>=64:raise ValueError('platform ADT recursion/node limit')
        nodes+=1;start=at
        count,children=struct.unpack_from('<II',data,at);at+=8
        name=None;cursor=None;first=None
        for i in range(count):
            key=data[at:at+32].split(b'\0')[0].decode('ascii')
            size=struct.unpack_from('<I',data,at+32)[0];value=at+36
            at=(value+size+3)&~3
            if i==0:first=key
            if key=='name':name=data[value:value+size];cursor=at
        if name==b'arm-io\0':
            if (depth!=1 or first!='name' or cursor!=start+52 or
                    data[start+8:start+40]!=b'name'.ljust(32,b'\0') or data[cursor-1]):
                raise ValueError('platform requires canonical root arm-io node')
            found.append((start,cursor))
        for _ in range(children):visit(depth+1)
    visit(0)
    if len(found)!=1:raise ValueError('platform requires one arm-io search result')
    return found[0]


def audit_metadata(data,kernel,adt):
    if hashlib.sha256(data).hexdigest()!=KERNEL_SHA256:
        raise ValueError('platform initialization requires exact audited source kernel')
    names=symbols(data,(*NAMES,*AUDITED,'_DTSize','_DTEnd','_DTInitialized'))
    for name,(size,digest) in AUDITED.items():
        at=backed(kernel.segments,names[name],size)
        if hashlib.sha256(data[at:at+size]).hexdigest()!=digest:
            raise ValueError('platform instruction audit changed: '+name)
    if tuple(names[k]-names['_DTRootNode'] for k in ('_DTSize','_DTEnd','_DTInitialized'))!=(8,16,24):
        raise ValueError('unsupported ADT global layout')
    return METADATA.pack(*(names[k] for k in NAMES),*search_offsets(adt)),names['_pe_arm_get_soc_base_phys']+0x34
