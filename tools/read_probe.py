#!/usr/bin/env python3
"""Read and validate the native probe text exported through Linux's device tree."""
import argparse
from pathlib import Path
import re

DEFAULT = Path('/sys/firmware/devicetree/base/chosen/trogdor-ipados,probe-log')


def read_record(data, xnu_checkpoint=False, xnu_mmu=False, xnu_highva=False, xnu_cinit=False, xnu_cpu=False, xnu_platform=False, xnu_pe=False, xnu_soc=False, xnu_topology=False):
    if xnu_topology: xnu_pe=True
    if xnu_pe and xnu_soc: raise ValueError('choose one PE or SoC log mode')
    xnu_platform = xnu_platform or xnu_pe or xnu_soc
    xnu_cpu = xnu_cpu or xnu_platform
    xnu_cinit = xnu_cinit or xnu_cpu
    if len(data) != 8192 or b'\0' not in data:
        raise ValueError('invalid probe log capacity or missing terminator')
    text, rest = data.split(b'\0', 1)
    if any(rest) or not text.startswith(b'Trogdor-ipadOS native phase-0 probe\n'):
        raise ValueError('empty, stale, or unrecognized probe log')
    text = text.decode('ascii')
    if any((xnu_checkpoint,xnu_mmu,xnu_highva,xnu_cinit,xnu_cpu,xnu_platform,xnu_pe,xnu_soc,xnu_topology)) and re.search(r"^Display ",text,re.M):
        raise ValueError("display discovery is separate from XNU acceptance")
    if not xnu_topology and any((xnu_checkpoint,xnu_mmu,xnu_highva,xnu_cinit,xnu_cpu,xnu_platform,xnu_pe,xnu_soc)) and (re.search(r'^XNU (topology schema|CPU topology verified|WFE state verified|system registers restored)=',text,re.M) or re.search(r'^XNU payload version=0x000000000000000b$',text,re.M)):
        raise ValueError('topology record requires its own strict mode')
    fields = {}
    for key in ('entry EL', 'current EL', 'stage', 'handoff EL'):
        values = re.findall(r'^' + re.escape(key) + r'=(0x[0-9a-f]{16})$', text, re.M)
        if len(values) != 1:
            raise ValueError('missing/duplicate probe field: ' + key)
        fields[key] = int(values[0], 16)
    if fields != {'entry EL': 2, 'current EL': 1, 'stage': 3, 'handoff EL': 2}:
        raise ValueError('probe did not complete the required EL2 -> EL1 -> EL2 path')
    if xnu_checkpoint or xnu_mmu or xnu_highva or xnu_cinit:
        for key in ('XNU copied', 'XNU checkpoint', 'XNU boot_args verified', 'XNU current EL'):
            values = re.findall(r'^' + re.escape(key) + r'=(0x[0-9a-f]{16})$', text, re.M)
            if len(values) != 1 or int(values[0], 16) != 1:
                raise ValueError('XNU checkpoint incomplete: ' + key)
            fields[key] = 1
    if xnu_mmu or xnu_highva or xnu_cinit:
        for key in ('XNU MMU enabled', 'XNU tables verified', 'XNU translations verified'):
            values = re.findall(r'^' + re.escape(key) + r'=(0x[0-9a-f]{16})$', text, re.M)
            if len(values) != 1 or int(values[0], 16) != 1:
                raise ValueError('XNU MMU checkpoint incomplete: ' + key)
            fields[key] = 1
    if xnu_highva:
        for key in ('XNU high VA verified', 'XNU stack read verified', 'XNU C entry verified'):
            values = re.findall(r'^' + re.escape(key) + r'=(0x[0-9a-f]{16})$', text, re.M)
            if len(values) != 1 or int(values[0], 16) != 1:
                raise ValueError('XNU high-VA checkpoint incomplete: ' + key)
            fields[key] = 1
    if xnu_cinit:
        for key in ('XNU C body verified', 'XNU stack writes verified', 'XNU args copy verified',
                    'XNU cache maintenance verified'):
            values = re.findall(r'^' + re.escape(key) + r'=(0x[0-9a-f]{16})$', text, re.M)
            if len(values) != 1 or int(values[0], 16) != 1:
                raise ValueError('XNU C initialization checkpoint incomplete: ' + key)
            fields[key] = 1
    if xnu_cpu:
        key = 'XNU CPU data verified'
        values = re.findall(r'^' + re.escape(key) + r'=(0x[0-9a-f]{16})$', text, re.M)
        if len(values) != 1 or int(values[0], 16) != 1:
            raise ValueError('XNU CPU initialization checkpoint incomplete: ' + key)
        fields[key] = 1
    if xnu_platform:
        for key in ('XNU platform state verified','XNU ADT binding verified','XNU ADT lookup verified'):
            values = re.findall(r'^' + re.escape(key) + r'=(0x[0-9a-f]{16})$', text, re.M)
            if len(values) != 1 or int(values[0],16) != 1:
                raise ValueError('XNU platform checkpoint incomplete: ' + key)
            fields[key] = 1
    if xnu_pe:
        for key in ('XNU SoC properties verified','XNU clock info verified','XNU PE return verified'):
            values = re.findall(r'^' + re.escape(key) + r'=(0x[0-9a-f]{16})$', text, re.M)
            if len(values)!=1 or int(values[0],16)!=1:
                raise ValueError('XNU PE return incomplete: '+key)
            fields[key]=1
    if xnu_soc:
        key='XNU SoC properties verified'
        values=re.findall(r'^'+re.escape(key)+r'=(0x[0-9a-f]{16})$',text,re.M)
        if len(values)!=1 or int(values[0],16)!=1: raise ValueError('XNU SoC properties incomplete')
        fields[key]=1
        if re.search(r'^XNU (clock info|PE return) verified=',text,re.M):
            raise ValueError('SoC midpoint must not claim later PE stages')
    if xnu_pe or xnu_soc:
        # Legacy mode7 records omit these; present diagnostic markers must be
        # unique and consistent with a strictly successful checkpoint.
        for key,want in (('XNU return boundary verified',1),('XNU diagnostic return',0)):
            values=re.findall(r'^'+re.escape(key)+r'=(.*)$',text,re.M)
            if values and (len(values)!=1 or not re.fullmatch(r'0x[0-9a-f]{16}',values[0]) or int(values[0],16)!=want):
                raise ValueError('XNU diagnostic record is not successful: '+key)
    if xnu_topology:
        def word(key):
            values=re.findall(r'^'+re.escape(key)+r'=(.*)$',text,re.M)
            if len(values)!=1 or not re.fullmatch(r'0x[0-9a-f]{16}',values[0]):
                raise ValueError('missing/duplicate/malformed topology field: '+key)
            fields[key]=int(values[0],16);return fields[key]
        for key in ('XNU WFE state verified','XNU CPU topology verified','XNU system registers restored','XNU topology schema'):
            if word(key)!=1:raise ValueError('topology checkpoint incomplete: '+key)
        if word('XNU return boundary verified')!=1 or word('XNU diagnostic return')!=0:
            raise ValueError('topology record is a failed return')
        if word('XNU payload version')!=11 or word('XNU ELR')!=0xfffffff0079b3d50 or word('XNU WFE EVENTI')!=4:
            raise ValueError('wrong topology version/PC/EVENTI')
        for name in ('CNTKCTL','TPIDR','TPIDRRO'):
            before,live,after=(word('XNU '+name+' '+stage) for stage in ('before','live','after'))
            if after!=before or live!=(before|0x4f if name=='CNTKCTL' else 0):
                raise ValueError('wrong live/restored topology register: '+name)
    return text, fields



def read_diagnostic_record(data):
    text,fields=read_record(data)
    if re.search(r"^Display ",text,re.M):raise ValueError("display discovery is separate from XNU diagnostics")
    def word(key):
        values=re.findall(r'^'+re.escape(key)+r'=(.*)$',text,re.M)
        if len(values)!=1 or not re.fullmatch(r'0x[0-9a-f]{16}',values[0]):
            raise ValueError('missing/duplicate/malformed diagnostic field: '+key)
        value=int(values[0],16);fields[key]=value;return value
    for key in ('XNU copied','XNU diagnostic schema','XNU diagnostic transport verified','XNU table pool verified'):
        if word(key)!=1:raise ValueError('diagnostic transport incomplete: '+key)
    version=word('XNU payload version')
    if version not in (9,10):raise ValueError('unknown diagnostic payload')
    capture=word('XNU capture verified');fault=word('XNU fault received')
    checkpoint=word('XNU checkpoint');returned=word('XNU diagnostic return')
    boundary=word('XNU return boundary verified');reason=word('XNU diagnostic reason')
    if any(v not in (0,1) for v in (capture,fault,checkpoint,returned,boundary)):
        raise ValueError('nonboolean diagnostic status')
    want=1 if fault else 2 if not capture else 3 if not checkpoint else 0
    if reason!=want or returned!=1-checkpoint or boundary!=int(capture and not fault) or (fault and (capture or checkpoint)):
        raise ValueError('contradictory diagnostic result')
    if checkpoint and (not capture or not boundary):raise ValueError('incomplete successful diagnostic')
    if version==10 and not fault:raise ValueError('deliberate fault control did not fault')
    regs=[word('XNU fault '+name) for name in ('ESR','ELR','FAR','SPSR')]
    if not fault and any(regs):raise ValueError('normal checkpoint has fault registers')
    if fault and ((regs[3]&31) not in (4,5) or regs[1]&3):raise ValueError('invalid original fault context')
    if version==10 and regs[0]!=(0xf2000000|0x5846):raise ValueError('wrong deliberate BRK syndrome')
    raw=[word(f'XNU capture {i:02}') for i in range(27)]
    if (raw[1]&31)!=(5 if fault else 4):raise ValueError('wrong diagnostic HVC origin')
    if fault:
        if raw[0]!=raw[15]+(4 if (regs[3]&31)==4 else 0x204):
            raise ValueError('wrong synchronous vector HVC')
        if version==10 and regs[1]!=0xfffffff007948410:raise ValueError('wrong deliberate fault PC')
    elif raw[0]!=0xfffffff007948498:raise ValueError('wrong SoC checkpoint HVC')
    if raw[25]<4 or raw[25]>131072 or raw[25]&(raw[25]-1) or raw[26]!=(24*1024*1024)//raw[25]:
        raise ValueError('incomplete diagnostic cache maintenance')
    # A successful diagnostic still has to pass every normal SoC field.
    if checkpoint:read_record(data,xnu_soc=True)
    return text,fields


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--input', type=Path, default=DEFAULT)
    parser.add_argument('--xnu-checkpoint', action='store_true')
    parser.add_argument('--xnu-mmu', action='store_true')
    parser.add_argument('--xnu-highva', action='store_true')
    parser.add_argument('--xnu-cinit', action='store_true')
    parser.add_argument('--xnu-cpu', action='store_true')
    parser.add_argument('--xnu-diagnostic', action='store_true')
    parser.add_argument('--xnu-soc', action='store_true')
    parser.add_argument('--xnu-pe', action='store_true')
    parser.add_argument('--display-discovery', action='store_true')
    parser.add_argument('--xnu-topology', action='store_true')
    parser.add_argument('--xnu-platform', action='store_true')
    args = parser.parse_args()
    with args.input.open('rb') as source:
        data = source.read(8193)
    if args.display_discovery:
        if any((args.xnu_checkpoint,args.xnu_mmu,args.xnu_highva,args.xnu_cinit,args.xnu_cpu,args.xnu_platform,args.xnu_pe,args.xnu_soc,args.xnu_topology,args.xnu_diagnostic)):
            parser.error("display discovery and XNU require separate strict modes")
        from read_display import read_display_record
        text, _ = read_display_record(data)
    elif args.xnu_diagnostic:
        if any((args.xnu_checkpoint,args.xnu_mmu,args.xnu_highva,args.xnu_cinit,args.xnu_cpu,args.xnu_platform,args.xnu_pe,args.xnu_soc,args.xnu_topology)):
            parser.error('choose diagnostic transport or strict acceptance mode')
        text, _ = read_diagnostic_record(data)
    else:
        text, _ = read_record(data, args.xnu_checkpoint, args.xnu_mmu, args.xnu_highva, args.xnu_cinit, args.xnu_cpu, args.xnu_platform, args.xnu_pe, args.xnu_soc, args.xnu_topology)
    print(text, end='')


if __name__ == '__main__':
    main()
