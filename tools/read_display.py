#!/usr/bin/env python3
"""Strictly read a complete read-only display discovery; never infer visibility."""
import re
from read_probe import read_record

REGISTERS=('TE','DSI','source size','output size','source XY','output XY','scanout PA',
           'scanout stride','format','unpack','GPIO config','GPIO IO','EC protocol')
METADATA=('table bytes','table entries','table framebuffer PA','width','height','stride','bpp','orientation',
          'Image PA','Image bytes','frame span','allocation bytes')
FLAGS=('scanout consistent','Coreboot RAM','FDT range allowed','Image conflict','candidate writable')
FIXED={'schema':1,'coreboot table verified':1,'framebuffer metadata verified':1,'GPIO metadata verified':1,
       'MMIO reads':13,'MMIO fault':0,'discovery complete':1,'hardware writes':0}

def read_display_record(data):
    text,fields=read_record(data)
    if re.search(r'^(XNU |EXCEPTION)',text,re.M):raise ValueError('display discovery must be separate from XNU/fault records')
    wanted=set(FIXED)|set(METADATA)|set(REGISTERS)|set(FLAGS)
    keys=re.findall(r'^Display ([^=\n]+)=',text,re.M)
    if set(keys)!=wanted or len(keys)!=len(wanted):raise ValueError('missing/duplicate/unknown display fields')
    values={}
    for key in wanted:
        found=re.findall(r'^Display '+re.escape(key)+r'=(0x[0-9a-f]{16})$',text,re.M)
        if len(found)!=1:raise ValueError('malformed display field: '+key)
        values[key]=int(found[0],16)
    if any(values[k]!=v for k,v in FIXED.items()):raise ValueError('display discovery is incomplete')
    if any(values[k] not in (0,1) for k in FLAGS):raise ValueError('nonboolean display result')
    if any(values[k]>0xffffffff for k in REGISTERS):raise ValueError('MMIO read is not 32 bits')
    w,h,stride=(values[k] for k in ('width','height','stride'))
    span=stride*h;allocation=(span+4095)&~4095
    if not (0<w<=8192 and 0<h<=8192 and w*4<=stride<=65536 and stride%4==0 and span<=64*1024*1024 and
            values['bpp']==32 and values['orientation']<=3 and values['frame span']==span and values['allocation bytes']==allocation and
            0<values['table bytes']<=65512 and values['table bytes']%4==0 and 0<values['table entries']<=256):
        raise ValueError('invalid display metadata')
    image,size,pa=(values[k] for k in ('Image PA','Image bytes','scanout PA'))
    if image%0x200000 or not 0x80000000<=image<1<<38 or not 0<size<=130*1024*1024 or image+size>1<<38:
        raise ValueError('invalid discovery Image extent')
    geometry=w|(h<<16)
    consistent=(values['source size']==values['output size']==geometry and values['source XY']==values['output XY']==0 and
        pa!=0 and pa%0x100000==0 and values['scanout stride']==stride and values['format']==0x236ff and values['unpack']==0x03020001 and
        values['table framebuffer PA'] in (0,pa))
    conflict=(pa<image+size and image<pa+allocation)
    if values['scanout consistent']!=consistent or values['Image conflict']!=conflict:
        raise ValueError('contradictory display snapshot/ownership')
    candidate=consistent and values['Coreboot RAM'] and values['FDT range allowed'] and not conflict
    if values['candidate writable']!=candidate:raise ValueError('contradictory display candidate')
    fields.update({'Display '+k:v for k,v in values.items()})
    return text,fields
