#!/usr/bin/env python3
"""Bind ShengHidInput's touch digitizer to the unchanged original IOHIDDevice, with the Coachz HID start-up."""
# This replaces UniXNU's script of the same name in the assembled tree (device scripts are copied last): the component recipe runs it.
# SPDX-License-Identifier: BSD-2-Clause-Patent
# ABI qualification adapted from UniXNU scripts/build-sheng-hid-input.py. The original IOHIDFamily of this OS profile lands at the
# same addresses in the Coachz prelinked kernel as in UniXNU's reference candidate: every check below runs against our own
# reference kernel (COACHZ_DISPLAY_REFERENCE), and the instruction hash must match.
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
ROOT=Path(__file__).resolve().parents[1];OUT=ROOT/'build/sheng-hid-input';OUT.mkdir(parents=True,exist_ok=True)
sys.path.insert(0,str(ROOT/'kits/XnuPrelink'))
from native_prelink import macho,backed_offset,exports
def run(cmd,**kw):return subprocess.check_output([str(x) for x in cmd],timeout=90,**kw)
def require(v,why):
    if not v:raise SystemExit(why)
def sha(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest()
HID_TEXT_SHA='7e4c5104814f2e773d5be13d092dec53ee44ba5a61a81264b229780406e86b72'
candidate=Path(os.environ['COACHZ_DISPLAY_REFERENCE'])
b=candidate.read_bytes();m=macho(b);plan=json.loads(candidate.with_suffix('.json').read_text())
require(plan['original_executable_sha256']['com.apple.iokit.IOHIDFamily']==HID_TEXT_SHA,'Exact original IOHIDFamily of this OS profile')
def read(a,n):
    at=backed_offset(m['segments'],a,n,len(b));return b[at:at+n]
names={n:a for a,ns in exports(b,m).items() for n in ns}
e=next(e for e in plan['extents'] if e['module']=='com.apple.iokit.IOHIDFamily' and e['segment']=='__TEXT_EXEC')
require(hashlib.sha256(read(e['va'],e['size'])).hexdigest()==HID_TEXT_SHA,'Complete original HID instructions unchanged')
vptr=0xfffffff005d045c0;methods=struct.unpack('<207Q',read(vptr,207*8))
base=struct.unpack('<168Q',read(names['__ZTV9IOService']+16,168*8))
changed=[i for i in range(168) if methods[i]!=base[i]]
require(changed==[0,1,7,12,14,21,23,76,81,82,86,87,91,92,93,101,132,140],'Exact original HID native-base overrides')
require([i for i,a in enumerate(methods) if a==names['___cxa_pure_virtual']]==[180] and all(methods) and read(vptr+207*8,24)==bytes(24),'Exact original 207-slot abstract device')
# The original protected constructor calls the native IOService constructor through its thunk, stores its vptr and returns.
require(read(0xfffffff004ad94f8,0x20)==bytes.fromhex('fd7bbfa9fd0300912bd90094489100f008011791080000f9fd7bc1a8c0035fd6'),'Original protected constructor and exact vptr store')
require(struct.unpack('<Q',read(0xfffffff005d00f10,8))[0]==names['__ZN9IOServiceC2EPK11OSMetaClass'],'Actual native protected constructor binding')
require(struct.unpack('<Q',read(0xfffffff005d00f18,8))[0]==names['__ZN9IOServiceD2Ev'],'Actual native protected destructor binding')
# Original start passes (self, provider) to slot 168, then (self, &descriptor) to pure slot 180 and checks the returned status.
require(read(0xfffffff004ad9aec,0x34)==bytes.fromhex('680240f908a142f9e00313aae10314aa00013fd6680240f9603d003608d142f9e1630091e00313aa00013fd6a02d0035e00f40f9'),'Original handleStart and report-descriptor ABI')
require(methods[181]==0xfffffff004add0c4 and read(0xfffffff004add110,28)==bytes.fromhex('680240f9081543f9e00313aae20316aae30315aae40314aa00013fd6'),'Original handleReport forwards timestamp, report, type and options')
header='''// Generated from the qualified original image; opaque private storage only.
#pragma once
#include <IOKit/IOService.h>
#include <IOKit/IOMemoryDescriptor.h>
class IOHIDDevice : public IOService {
 OSDeclareAbstractStructors(IOHIDDevice)
 unsigned char originalPrivate[0xd0-sizeof(IOService)];
public:
 using IOService::setProperty;
 IOReturn Dispatch(IORPC) override;
 void free() override;
 bool init(OSDictionary *properties=nullptr) override;
 bool setProperty(const OSSymbol *,OSObject *) override;
 bool willTerminate(IOService *,IOOptionBits) override;
 IOReturn configureReport(IOReportChannelList *,IOReportConfigureAction,void *,void *) override;
 IOReturn updateReport(IOReportChannelList *,IOReportUpdateAction,void *,void *) override;
 bool start(IOService *) override;
 void stop(IOService *) override;
 bool handleOpen(IOService *,IOOptionBits,void *) override;
 void handleClose(IOService *,IOOptionBits) override;
 bool handleIsOpen(const IOService *) const override;
 bool matchPropertyTable(OSDictionary *,SInt32 *) override;
 IOReturn message(UInt32,IOService *,void *) override;
 IOReturn newUserClient(task_t,void *,UInt32,OSDictionary *,IOUserClient **) override;
'''
for i in range(168,207):
    if i==171:header+=' virtual OSString *newTransportString() const;\n'
    elif i==173:header+=' virtual OSString *newProductString() const;\n'
    elif i==180:header+=' virtual IOReturn newReportDescriptor(IOMemoryDescriptor **) const=0;\n'
    elif i==181:header+=' virtual IOReturn handleReport(IOMemoryDescriptor *,unsigned,IOOptionBits);\n'
    else:header+=f' virtual __attribute__((deprecated("Unqualified prototype: never call from source"))) void opaque{i}();\n'
header+='};\nstatic_assert(sizeof(IOHIDDevice)==0xd0,"Original device instance size");\n'
(OUT/'OriginalHidDeviceABI.h').write_text(header)
print(run(['python3',ROOT/'scripts/build-arm-driver.py','ShengHidInput'],text=True),end='')
obj=OUT/'NativeHidInput.o';raw=obj.read_bytes();om=macho(raw)
st=next(p for c,p,_ in om['commands'] if c==2);sy,count,strings,_=struct.unpack_from('<4I',raw,st+8);symbols=[]
for i in range(count):
    ix,kind,section,_,va=struct.unpack_from('<IBBHQ',raw,sy+i*16)
    symbols.append((raw[strings+ix:raw.index(b'\0',strings+ix)].decode(),va,kind))
vt=next(va for name,va,_ in symbols if name=='__ZTV14NativeHidInput');relocations={}
for segment in om['segments']:
    for sec in segment['sections']:
        if sec['va']<=vt<sec['va']+sec['size']:
            ro,rc=struct.unpack_from('<2I',raw,sec['command_offset']+56)
            for i in range(rc):
                offset,bits=struct.unpack_from('<2I',raw,ro+i*8)
                if bits>>27&1 and bits>>28==0 and bits>>25&3==3:relocations[sec['va']+offset]=symbols[bits&0xffffff][0]
compiled=[relocations.get(vt+16+i*8) for i in range(207)]
require(all(compiled) and vt+16+207*8 not in relocations,'Compiled exact 207-slot vtable')
bindings={'__ZN11IOHIDDeviceC2EPK11OSMetaClass':0xfffffff004ad94f8,'__ZN11IOHIDDeviceD2Ev':0xfffffff004ad9518,'__ZN11IOHIDDevice10gMetaClassE':0xfffffff006ae6fa0,'__ZTV11IOHIDDevice':vptr-16}
owned=[]
for i,name in enumerate(compiled):
    if name.startswith(('__ZN14NativeHidInput','__ZNK14NativeHidInput')):owned.append(i)
    elif '11IOHIDDevice' in name:bindings[name]=methods[i]
    else:require(i<168 and names.get(name)==methods[i],'Actual inherited native method '+str(i))
require(owned==[0,1,7,14,86,87,111,171,173,180],'Only qualified HID methods overridden')
undefined={n for n,_,k in symbols if k==1 and '11IOHIDDevice' in n}
require(undefined==set(bindings),'All and only actual original HID imports bound')
asm=OUT/'hid-original-bindings.S';asm.write_text(''.join(f'.globl {n}\n.set {n}, {a:#x}\n' for n,a in sorted(bindings.items())))
sdk=run(['xcrun','--sdk','iphoneos','--show-sdk-path'],text=True).strip()
run(['xcrun','clang','-target','arm64-apple-ios17.0','-isysroot',sdk,'-c',asm,'-o',OUT/'hid-original-bindings.o'])
from coachz_late_init import stage_initializer
late=OUT/'NativeHidInput.late.o'
late.write_bytes(stage_initializer(raw,'__GLOBAL__sub_I_NativeHidInput.cpp'))
def code(data,info):
    sec=next(s for g in info['segments'] for s in g['sections'] if s['name']=='__text');return data[sec['fileoff']:sec['fileoff']+sec['size']]
require(code(raw,om)==code(late.read_bytes(),macho(late.read_bytes())),'Owned code unchanged by deferred metaclass registration')

# The Coachz start-ups (kits/coachz), compiled like the display glue with the platform expert's flags.
record=json.loads((ROOT/'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json').read_text().strip().rstrip(','))
def compile_glue(source,output):
    args=record['arguments'];cmd=[];i=0
    while i<len(args):
        value=args[i]
        if value==record['file']:cmd.append(str(source))
        elif value in ('-o','-MF','-MT','-MJ'):
            if value=='-o':cmd+=['-o',str(output)]
            i+=1
        elif value=='-Xclang' and args[i+1].startswith('-W'):i+=1
        elif not value.startswith('-W'):cmd.append(value)
        i+=1
    cmd+=['-Wall','-Wextra','-Werror','-Wno-unused-parameter','-Wno-unnecessary-virtual-specifier','-Wno-c99-designator','-Wno-reorder-init-list','-Wno-deprecated-enum-enum-conversion']
    run(cmd,cwd=record['directory'])
source=ROOT/'kits/coachz/CoachzHid.cpp'
compile_glue(source,OUT/'CoachzHid.o')
# UniXNU's AppleKeyStoreState service (component:AppleKeyStoreState runs first) with its class registered at start-up, as in UniXNU-sheng.
keystore=ROOT/'kits/coachz/CoachzKeyStore.cpp'
compile_glue(keystore,OUT/'CoachzKeyStore.o')
keyevents=(ROOT/'build/apple-key-store-state/NativeKeyStoreEvents.o').read_bytes()
keyevents_late=OUT/'NativeKeyStoreEvents.late.o'
keyevents_late.write_bytes(stage_initializer(keyevents,'__GLOBAL__sub_I_NativeKeyStoreEvents.cpp'))
require(code(keyevents,macho(keyevents))==code(keyevents_late.read_bytes(),macho(keyevents_late.read_bytes())),'Owned keystore code unchanged by deferred metaclass registration')
archive=OUT/'libCoachzHid.a';archive.unlink(missing_ok=True)
run(['xcrun','ar','rcs',archive,late,OUT/'hid-original-bindings.o',OUT/'CoachzHid.o',keyevents_late,OUT/'CoachzKeyStore.o'])
report={'archive_sha256':sha(archive),'reference_sha256':sha(candidate),'original_text_sha256':HID_TEXT_SHA,'original_slots':207,
        'owned_slots':owned,'bindings':{n:hex(v) for n,v in bindings.items()},'source_sha256':{str(p.relative_to(ROOT)):sha(p) for p in
        [ROOT/'drivers/ShengHidInput/NativeHidInput.cpp',source,ROOT/'drivers/AppleKeyStoreState/NativeKeyStoreEvents.cpp',keystore]},'hardware_accepted':False}
(OUT/'coachz-report.json').write_text(json.dumps(report,indent=2)+'\n')
print('Coachz: original IOHIDDevice ABI bound and touch digitizer built; hardware not accepted')
