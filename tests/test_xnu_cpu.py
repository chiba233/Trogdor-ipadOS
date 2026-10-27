import ctypes
import hashlib
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
from unittest.mock import patch

import test_xnu_checkpoint as entrytests
import test_xnu_cinit as cinittests
from test_xnu_checkpoint import ROOT, BASE, ARENA, Expected, adt
from xnu_checkpoint import pack, HEADER, HVC, symbols
from xnu_cpu import audit_metadata
from xnu_macho import macho, backed
from linux_log import compile_wrapper
from read_probe import read_record


def synthetic_payload():
    (prior,_),data=cinittests.synthetic_payload()
    at=HEADER.size+HEADER.unpack_from(prior)[2]*40
    with patch('xnu_checkpoint.MMU_PREFIX_SHA256',hashlib.sha256(data[0x1100:0x1400]).hexdigest()), \
         patch('xnu_checkpoint.HIGHVA_PREFIX_SHA256',hashlib.sha256(data[0x1100:0x1430]).hexdigest()), \
         patch('xnu_cinit.audit_metadata',return_value=prior[at+48:at+96]), \
         patch('xnu_cpu.audit_metadata',return_value=struct.pack('<2Q',BASE+0x5000,BASE+0x4980)):
        return pack(data,adt(),cpu_init=True),data


def capture(e):
    values=cinittests.capture(e)
    values[18]=values[19]=e.cpu_data
    values[22]=e.virt+e.pc-e.phys
    return values


def replay_cpu_leaf(code, pc, target):
    """Independent small instruction interpreter for the optional exact source.

    No CPU store range table is shared with the C validator. Unknown opcodes,
    calls, MMIO, out-of-object stores and missing final RET are all rejected.
    """
    gpr=[0]*32;gpr[0]=target
    vectors=[bytes(16) for _ in range(32)]
    memory=bytearray(b'\xa5'*0x500);vector_stores=0
    def reg(i):return gpr[i] if i!=31 else 0
    def store(address,value):
        offset=address-target
        if not 0<=offset<=len(memory)-len(value):raise AssertionError('store outside CPU object')
        memory[offset:offset+len(value)]=value
    def number(value,n):return (value&((1<<(8*n))-1)).to_bytes(n,'little')
    for offset in range(0,len(code),4):
        word=struct.unpack_from('<I',code,offset)[0]
        rt,rn=word&31,(word>>5)&31
        if word==0xd65f03c0:
            if offset+4!=len(code):raise AssertionError('early RET')
            return memory,vector_stores
        if word==0x9e6703e0: # FMOV D0,XZR, no FP arithmetic
            vectors[0]=bytes(16);continue
        if word&~31==0x4f00e400: # MOVI Vd.16B,#0
            vectors[rt]=bytes(16);continue
        if word==0x6e180440: # INS V0.D[1],V2.D[0]
            vectors[0]=vectors[0][:8]+vectors[2][:8];continue
        move=word&0x7f800000
        if move in (0x12800000,0x52800000,0x72800000):
            bits=64 if word>>31 else 32;shift=((word>>21)&3)*16
            imm=((word>>5)&0xffff)<<shift;mask=(1<<bits)-1
            if move==0x12800000:gpr[rt]=~imm&mask
            elif move==0x52800000:gpr[rt]=imm
            else:gpr[rt]=(reg(rt)&~(0xffff<<shift)|imm)&mask
            continue
        if word&0x9f000000==0x90000000:
            imm=(((word>>5)&0x7ffff)<<2)|((word>>29)&3)
            if imm&(1<<20):imm-=1<<21
            gpr[rt]=((pc+offset)&~4095)+imm*4096;continue
        if word&0xff000000==0x91000000:
            gpr[rt]=reg(rn)+((word>>10)&4095)*(4096 if word&(1<<22) else 1);continue
        kind=word&0xffc00000
        sizes={0x79000000:2,0xb9000000:4,0xf9000000:8,0x3d800000:16}
        if kind in sizes:
            size=sizes[kind];address=reg(rn)+((word>>10)&4095)*size
            store(address,vectors[rt] if size==16 else number(reg(rt),size))
            vector_stores+=size==16;continue
        if word&0xffe00c00==0x3c800000: # STUR Q
            imm=(word>>12)&511
            if imm&256:imm-=512
            store(reg(rn)+imm,vectors[rt]);vector_stores+=1;continue
        if kind in (0xa9000000,0xad000000): # STP X/X or Q/Q, signed offset
            size=8 if kind==0xa9000000 else 16
            imm=(word>>15)&127
            if imm&64:imm-=128
            rt2=(word>>10)&31
            values=vectors[rt]+vectors[rt2] if size==16 else number(reg(rt),8)+number(reg(rt2),8)
            store(reg(rn)+imm*size,values);vector_stores+=2 if size==16 else 0;continue
        raise AssertionError(f'unknown CPU-leaf instruction {word:08x}')
    raise AssertionError('missing RET')


class XnuCpuTests(unittest.TestCase):
    setUpClass=classmethod(entrytests.XnuCheckpointTests.setUpClass.__func__)
    tearDownClass=classmethod(entrytests.XnuCheckpointTests.tearDownClass.__func__)
    load=entrytests.XnuCheckpointTests.load

    def test_version5_patch_and_seed_are_separate_from_cinit(self):
        (payload,report),data=synthetic_payload()
        raw=HEADER.unpack_from(payload)[7]
        changed=[i for i,(a,b) in enumerate(zip(data,payload[raw:raw+len(data)])) if a!=b]
        self.assertTrue(changed)
        self.assertTrue(all(0x21a4<=i<0x21a8 for i in changed))
        self.assertTrue(report['cpu_checkpoint'])
        good,_,e=self.load(payload);self.assertEqual(good,1)
        self.assertEqual(e.mode,5)
        address=e.phys+e.cpu_data-e.virt
        self.assertEqual(ctypes.string_at(address-16,0x520),bytes(16)+b'\xa5'*0x500+bytes(16))
        self.assertEqual(e.rtclock,BASE+0x4980)
        self.assertEqual(struct.unpack_from('<I',payload,raw+0x21a4)[0],HVC)

    def test_invalid_cpu_ranges_source_guards_or_overlap_never_write(self):
        (payload,_),_=synthetic_payload();h=HEADER.unpack_from(payload)
        ext=HEADER.size+h[2]*40;raw=h[7]
        bads=[]
        for offset,value in ((ext+96,BASE),(ext+96,BASE+0x2000),(ext+96,BASE+0x4400),
                             (ext+96,BASE+0x8000),(ext+96,BASE+0x1b000),
                             (ext+96,BASE+0x5008),(ext+104,BASE+0x5000),
                             (ext+104,BASE+0x2000),(ext+104,BASE+0x30000)):
            b=bytearray(payload);struct.pack_into('<Q',b,offset,value);bads.append(b)
        # Shift CPU object into file-backed zero area, while keeping other globals disjoint.
        for off in (0x4c00-16,0x4c00,0x4c00+0x300):
            b=bytearray(payload);struct.pack_into('<Q',b,ext+96,BASE+0x4c00)
            b[raw+off]=1;bads.append(b)
        for index,bad in enumerate(bads):
            with self.subTest(case=index):
                ctypes.memset(self.address,0xaa,0x30000)
                self.assertEqual(self.load(bad)[0],0)
                self.assertEqual(ctypes.string_at(self.address,0x30000),b'\xaa'*0x30000)

    def test_cpu_capture_every_field_and_old_c_registers_reject(self):
        (payload,_),_=synthetic_payload();good,_,e=self.load(payload);self.assertEqual(good,1)
        api=self.lib.xnu_capture_valid
        api.argtypes=[ctypes.POINTER(ctypes.c_uint64),ctypes.POINTER(Expected)]
        def valid(v):return api((ctypes.c_uint64*27)(*v),ctypes.byref(e))
        values=capture(e);self.assertEqual(valid(values),1)
        for i in range(27):
            bad=values.copy();bad[i]^=1
            with self.subTest(register=i):self.assertEqual(valid(bad),0)
        for i,value in ((18,e.const_args),(19,e.virt+e.args-e.phys),(22,values[22]-8),
                        (2,values[2]|2),(20,0)):
            bad=values.copy();bad[i]=value;self.assertEqual(valid(bad),0)

    def check_cpu_output(self,e,output):
        address=e.phys+e.cpu_data-e.virt
        api=self.lib.xnu_cpu_valid;api.argtypes=[ctypes.POINTER(Expected)]
        self.assertEqual(api(ctypes.byref(e)),0) # initial seed cannot pass
        ctypes.memmove(address,bytes(output),len(output))
        self.assertEqual(api(ctypes.byref(e)),1)
        for i in range(-16,0x510):
            byte=ctypes.c_uint8.from_address(address+i);old=byte.value;byte.value^=1
            with self.subTest(byte=i):self.assertEqual(api(ctypes.byref(e)),0)
            byte.value=old
        for key,value in (('cpu_data',e.virt),('cpu_data',e.virt+1),('cpu_data',e.virt+ARENA),
                          ('rtclock',0),('end',e.phys-1),('mode',4)):
            old=getattr(e,key);setattr(e,key,value)
            self.assertEqual(api(ctypes.byref(e)),0);setattr(e,key,old)

    def test_cpu_byte_model_and_guards_reject_every_byte_corruption(self):
        (payload,_),_=synthetic_payload();good,_,e=self.load(payload);self.assertEqual(good,1)
        # Negative content/guard fixture; independent instruction-based model
        # coverage uses the optional real source below.
        output=bytearray(b'\xa5'*0x500)
        for at,size in ((2,2),(0x44,20),(0x68,84),(0xc0,8),(0x158,8),(0x170,80),
                        (0x1d0,32),(0x260,8),(0x290,12),(0x2a0,263)):
            output[at:at+size]=bytes(size)
        for at,fmt,value in ((0x40,'<I',0x80000000),(0x100,'<Q',2**64-1),
                             (0x150,'<Q',e.rtclock),(0x1cc,'<I',0x50535553),(0x280,'<I',2**32-1)):
            struct.pack_into(fmt,output,at,value)
        self.check_cpu_output(e,output)

    def test_exact_source_instruction_replay_matches_every_expected_byte(self):
        if not cinittests.SOURCE.exists():self.skipTest('optional pinned source absent')
        data=cinittests.SOURCE.read_bytes();payload,report=pack(data,adt(),cpu_init=True)
        good,_,e=self.load(payload);self.assertEqual(good,1)
        names=symbols(data,('_arm_init',));kernel=macho(data)
        at=backed(kernel.segments,names['_cpu_data_init'],0xc4)
        output,vectors=replay_cpu_leaf(data[at:at+0xc4],names['_cpu_data_init'],e.cpu_data)
        self.assertGreater(vectors,16)
        self.check_cpu_output(e,output)
        # The existing C-body/stack contract also remains valid after the leaf returns.
        cinittests.XnuCinitTests.populate_c_results(self,e)
        ctypes.c_uint64.from_address(e.phys+e.sp0-8-e.virt).value=e.c_entry+0x58c
        self.lib.xnu_cinit_valid.argtypes=[ctypes.POINTER(Expected)]
        self.assertEqual(self.lib.xnu_cinit_valid(ctypes.byref(e)),1)
        raw=HEADER.unpack_from(payload)[7]
        differences=[i for i,(a,b) in enumerate(zip(data,payload[raw:raw+len(data)])) if a!=b]
        self.assertTrue(differences)
        self.assertTrue(all(report['patch_file_offset']<=i<report['patch_file_offset']+4 for i in differences))
        for kwargs in ({},{'mmu':True},{'highva':True},{'cinit':True}):
            self.assertEqual(self.load(pack(data,adt(),**kwargs)[0])[0],1)
        bad=bytearray(data);bad[at]^=1
        with self.assertRaises(ValueError):pack(bad,adt(),cpu_init=True)

    def test_exact_audit_rejects_synthetic_or_changed_code(self):
        (_, _),data=synthetic_payload()
        with self.assertRaisesRegex(ValueError,'exact independently audited'):audit_metadata(data,macho(data))

    def test_reader_requires_cpu_field_and_all_fifteen_cinit_fields(self):
        keys={'entry EL':2,'current EL':1,'stage':3,'handoff EL':2,
              'XNU copied':1,'XNU checkpoint':1,'XNU boot_args verified':1,'XNU current EL':1,
              'XNU MMU enabled':1,'XNU tables verified':1,'XNU translations verified':1,
              'XNU C body verified':1,'XNU stack writes verified':1,'XNU args copy verified':1,
              'XNU cache maintenance verified':1,'XNU CPU data verified':1}
        def record(items):
            text=('Trogdor-ipadOS native phase-0 probe\n'+''.join(f'{k}=0x{v:016x}\n' for k,v in items)).encode()
            return text+bytes(8192-len(text))
        self.assertEqual(read_record(record(keys.items()),xnu_cpu=True)[1],keys)
        for key in keys:
            for how in ('missing','zero','duplicate'):
                items=[(k,v) for k,v in keys.items() if k!=key]
                if how=='zero':items.append((key,0))
                if how=='duplicate':items.extend([(key,keys[key])]*2)
                with self.subTest(key=key,how=how),self.assertRaises(ValueError):
                    read_record(record(items),xnu_cpu=True)

    def test_crosscompile_fp_enable_is_synchronized_and_cleanup_still_scalar(self):
        (payload,_),_=synthetic_payload()
        linux=struct.pack('<II6QII',0x91005a4d,0x14000001,0,4096,0xa,0,0,0,0x644d5241,0)+bytes(64)
        with tempfile.TemporaryDirectory() as tmp:
            image,report=compile_wrapper(Path(tmp),linux,4096,payload)
            self.assertEqual(report['xnu_checkpoint_version'],5)
            self.assertEqual(image[report['linux_offset']:],linux)
            self.assertLessEqual(report['header']['memory_size'],80*1024*1024)
            text=subprocess.check_output(['xcrun','llvm-objdump','-d',str(Path(tmp)/'probe.elf')],text=True)
            branch=text.split('<xnu_branch>:',1)[0]
            self.assertIn('msr\tCPACR_EL1, x10',branch)
            after=branch.rsplit('msr\tCPACR_EL1, x10',1)[1]
            self.assertIn('isb',after)
            self.assertIn('<xnu_clean_arena>:',text)
            self.assertIn('dc\tcivac',text)
