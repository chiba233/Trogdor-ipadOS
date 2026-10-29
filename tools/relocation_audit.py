"""Reject absolute symbol addresses in movable AArch64 C objects."""
from pathlib import Path
import struct
import subprocess


def inspect_object(data):
    if len(data)<64 or data[:6]!=b'\x7fELF\x02\x01' or struct.unpack_from('<HH',data,16)!=(1,183):
        raise ValueError('expected little-endian AArch64 relocatable object')
    at=struct.unpack_from('<Q',data,40)[0];stride,count=struct.unpack_from('<HH',data,58)
    if not count or stride<64 or at>len(data) or count>(len(data)-at)//stride:
        raise ValueError('invalid relocation section table')
    sections=[struct.unpack_from('<II4QII2Q',data,at+i*stride) for i in range(count)]
    checked=0
    for section in sections:
        if section[1] not in (4,9) or not section[5]:continue
        target=section[7]
        if target>=count:raise ValueError('invalid relocation target section')
        if not sections[target][2]&2:continue # Non-loaded debug references are harmless.
        record=24 if section[1]==4 else 16
        offset,size,step=section[4],section[5],section[9]
        if step!=record or size%step or offset>len(data) or size>len(data)-offset:
            raise ValueError('invalid relocation records')
        for p in range(offset,offset+size,step):
            site,info=struct.unpack_from('<QQ',data,p);kind=info&0xffffffff;checked+=1
            # Full absolute data addresses and absolute MOVW symbol fragments.
            # ADRP/ADD/LDST page offsets remain valid for our 2-MiB load alignment.
            if kind in (257,258,259) or 263<=kind<=272:
                raise ValueError(f'absolute symbol relocation {kind} at section {target} offset {site:#x}')
    return {'allocated_relocations_checked':checked,'absolute_symbol_relocations':0}


def audit_sources(zig,cpu,sources,env,out,defines=()):
    out=Path(out);out.mkdir(parents=True,exist_ok=True);reports={}
    flags=['-target','aarch64-freestanding-none','-mcpu='+cpu,'-mgeneral-regs-only',
           '-ffreestanding','-fno-builtin','-fno-pic','-fno-pie','-fno-stack-protector',
           '-fno-unwind-tables','-fno-asynchronous-unwind-tables','-Os','-g']
    for source in sources:
        source=Path(source);obj=out/(source.stem+'.o')
        subprocess.run([str(zig),'cc',*flags,*['-D'+d for d in defines],'-c',str(source),'-o',str(obj)],env=env,check=True)
        try:reports[source.name]=inspect_object(obj.read_bytes())
        except ValueError as error:raise ValueError(source.name+': '+str(error)) from error
    return reports
