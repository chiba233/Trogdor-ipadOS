#!/usr/bin/env python3
"""Strictly read a complete read-only display discovery; never infer visibility."""
import re
from read_probe import read_record

REGISTERS=('TE','DSI','source size','output size','source XY','output XY','scanout PA',
           'scanout stride','format','unpack','GPIO config','GPIO IO','EC protocol')
METADATA=('table bytes','table entries','table framebuffer PA','width','height','stride','bpp','orientation',
          'Image PA','Image bytes','frame span','allocation bytes')
FLAGS=('metadata consistent','allocation relation','allocator window','Coreboot RAM','FDT owners clear',
       'Image conflict','ownership verified','candidate writable')
FIXED={'schema':2,'coreboot table verified':1,'framebuffer metadata verified':1,'GPIO metadata verified':1,
       'MMIO reads':13,'MMIO fault':0,'discovery complete':1,'hardware writes':0}
# Historical records keep their original meaning; they are never reinterpreted.
SCHEMA1_FLAGS=('scanout consistent','Coreboot RAM','FDT range allowed','Image conflict','candidate writable')
SCHEMA1_FIXED=dict(FIXED,schema=1)
PATTERN=('pattern attempted','pattern mismatches','pattern hold seconds','pattern MMIO fault','pattern count enable saved',
         'pattern TE running','pattern DSI running','pattern frame start','pattern frame end','pattern line start','pattern line end',
         'pattern TE stopped','pattern DSI stopped')
BACKLIGHT=('backlight hello','backlight EC on','backlight EC off','backlight error','backlight GPIO on','backlight GPIO off')
DEPTHCHARGE_BASE=0xf1000000

def parse(data,fixed,flags,extra=()):
    text,fields=read_record(data)
    if re.search(r'^(XNU |EXCEPTION)',text,re.M):raise ValueError('display discovery must be separate from XNU/fault records')
    wanted=set(fixed)|set(METADATA)|set(REGISTERS)|set(flags)|set(extra)
    keys=re.findall(r'^Display ([^=\n]+)=',text,re.M)
    if set(keys)!=wanted or len(keys)!=len(wanted):raise ValueError('missing/duplicate/unknown display fields')
    values={}
    for key in wanted:
        found=re.findall(r'^Display '+re.escape(key)+r'=(0x[0-9a-f]{16})$',text,re.M)
        if len(found)!=1:raise ValueError('malformed display field: '+key)
        values[key]=int(found[0],16)
    if any(values[k]!=v for k,v in fixed.items()):raise ValueError('display discovery is incomplete')
    if any(values[k] not in (0,1) for k in flags):raise ValueError('nonboolean display result')
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
    if values['Image conflict']!=(pa<image+size and image<pa+allocation):
        raise ValueError('contradictory display snapshot/ownership')
    fields.update({'Display '+k:v for k,v in values.items()})
    return text,fields,values

def metadata_consistent(v):
    pa=v['scanout PA'];geometry=v['width']|(v['height']<<16)
    return (v['source size']==v['output size']==geometry and v['source XY']==v['output XY']==0 and
        pa!=0 and v['scanout stride']==v['stride'] and v['format']==0x236ff and v['unpack']==0x03020001 and
        v['table framebuffer PA'] in (0,pa))

def check_gates(v):
    pa=v['scanout PA'];end=pa+v['allocation bytes']
    metadata=metadata_consistent(v)
    relation=v['table framebuffer PA']==0 and end%0x100000==0
    window=pa>=DEPTHCHARGE_BASE and end<=1<<32
    if (v['metadata consistent'],v['allocation relation'],v['allocator window'])!=(metadata,relation,window):
        raise ValueError('contradictory display snapshot/ownership')
    owned=window and v['Coreboot RAM'] and v['FDT owners clear'] and not v['Image conflict']
    if v['ownership verified']!=owned:raise ValueError('contradictory display ownership')
    if v['candidate writable']!=(metadata and relation and owned):raise ValueError('contradictory display candidate')

def read_display_record(data):
    text,fields,v=parse(data,FIXED,FLAGS)
    check_gates(v)
    return text,fields

def parse_pattern(data,schema,extra=()):
    """Accepts any completed attempt; whether the timing engine ran is read from the counters."""
    fixed={k:b for k,b in FIXED.items() if k!='hardware writes'};fixed['schema']=schema
    text,fields,v=parse(data,fixed,FLAGS,PATTERN+tuple(extra)+('hardware writes',))
    check_gates(v)
    if v['pattern attempted']!=v['candidate writable']:raise ValueError('pattern must follow the candidate')
    if v['hardware writes']==0:
        if any(v[k] for k in PATTERN if k not in ('pattern attempted','pattern mismatches','pattern count enable saved','pattern MMIO fault')):
            raise ValueError('pattern reported without hardware writes')
    elif (v['pattern hold seconds'],v['pattern TE stopped'],v['pattern DSI stopped'])!=(20,0,0):
        raise ValueError('pattern did not stop cleanly')
    return text,fields,v

def read_display_pattern_record(data):
    text,fields,v=parse_pattern(data,3)
    if v['hardware writes'] not in (0,7):raise ValueError('unexpected pattern hardware writes')
    return text,fields

def read_display_backlight_record(data):
    """An EC failure is a valid record: the scanout still ran and the error code says why."""
    text,fields,v=parse_pattern(data,4,BACKLIGHT)
    hello,on,off=(v['backlight '+k] for k in ('hello','EC on','EC off'))
    if any(x not in (0,1) for x in (hello,on,off)) or (on and not hello) or (off and not on):
        raise ValueError('contradictory backlight sequence')
    if v['hardware writes']!=(9 if on else 7 if v['pattern hold seconds'] else 0):raise ValueError('unexpected backlight hardware writes')
    return text,fields

def read_display_record_schema1(data):
    text,fields,v=parse(data,SCHEMA1_FIXED,SCHEMA1_FLAGS)
    consistent=metadata_consistent(v) and v['scanout PA']%0x100000==0
    if v['scanout consistent']!=consistent:raise ValueError('contradictory display snapshot/ownership')
    candidate=consistent and v['Coreboot RAM'] and v['FDT range allowed'] and not v['Image conflict']
    if v['candidate writable']!=candidate:raise ValueError('contradictory display candidate')
    return text,fields
