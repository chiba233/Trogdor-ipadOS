import ctypes
import hashlib
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch

import test_xnu_checkpoint as entrytests
import test_xnu_mmu as mmutests
from test_xnu_checkpoint import ROOT, BASE, ARENA, Expected, adt, adrp
from xnu_checkpoint import pack, HEADER, symbols
from linux_log import compile_wrapper
from read_probe import read_record


def synthetic_kernel():
    data=mmutests.synthetic_kernel()
    # Extend the declared synthetic text section to include our fake C entry.
    struct.pack_into('<Q',data,32+72+40,0x1000)
    at=32
    while True:
        command,size=struct.unpack_from('<II',data,at)
        if command==2:break
        at+=size
    so,count,strings,length=struct.unpack_from('<4I',data,at+8)
    for i in range(count):
        name=struct.unpack_from('<I',data,so+16*i)[0]
        key=data[strings+name:data.find(b'\0',strings+name)]
        if key==b'_intstack_top':struct.pack_into('<Q',data,so+16*i+8,BASE+0x1b000)
        if key==b'_excepstack_top':struct.pack_into('<Q',data,so+16*i+8,BASE+0x1a000)
    data[strings+length:strings+length+10]=b'_arm_init\0'
    struct.pack_into('<IBBHQ',data,so+16*count,length,0x1e,1,0,BASE+0x1c00)
    # A STAB with the same spelling must not hide the real private symbol.
    struct.pack_into('<IBBHQ',data,so+16*(count+1),length,0x24,1,0,0)
    struct.pack_into('<4I',data,at+8,so,count+2,strings,length+10)
    struct.pack_into('<II',data,0x1168,adrp(30,BASE+0x1168,BASE+0x1c00),
                     0x910003de | (0xc00<<10))
    struct.pack_into('<I',data,0x1100+0x32c,0xd65f03c0)
    struct.pack_into('<3I',data,0x1c00,0xd10343ff,0xa9076ffc,0xa90867fa)
    return data


def synthetic_pack(data=None):
    original=synthetic_kernel()
    mmu_hash=hashlib.sha256(original[0x1100:0x1400]).hexdigest()
    high_hash=hashlib.sha256(original[0x1100:0x1430]).hexdigest()
    with patch('xnu_checkpoint.MMU_PREFIX_SHA256',mmu_hash),patch('xnu_checkpoint.HIGHVA_PREFIX_SHA256',high_hash):
        return pack(original if data is None else data,adt(),highva=True)


class XnuHighVaTests(unittest.TestCase):
    setUpClass=classmethod(entrytests.XnuCheckpointTests.setUpClass.__func__)
    tearDownClass=classmethod(entrytests.XnuCheckpointTests.tearDownClass.__func__)
    load=entrytests.XnuCheckpointTests.load

    def test_private_symbol_and_two_instruction_patch_preserve_original_sub(self):
        data=synthetic_kernel()
        self.assertNotIn('_arm_init',symbols(data))
        self.assertEqual(symbols(data,('_arm_init',))['_arm_init'],BASE+0x1c00)
        payload,report=synthetic_pack(data);h=HEADER.unpack_from(payload);raw=h[7]
        self.assertEqual(h[1],3)
        modified=payload[raw:raw+len(data)]
        differences=[i for i,(a,b) in enumerate(zip(data,modified)) if a!=b]
        self.assertTrue(differences)
        self.assertTrue(all(0x1c04<=i<0x1c0c for i in differences))
        self.assertEqual(modified[0x1c00:0x1c04],data[0x1c00:0x1c04])
        self.assertEqual(modified[0x1130:0x1134],data[0x1130:0x1134])
        self.assertEqual(modified[0x13fc:0x1400],data[0x13fc:0x1400])
        self.assertEqual(report['patch_bytes'],8)
        good,_,e=self.load(payload)
        self.assertEqual(good,1)
        self.assertEqual(e.mode,3)
        self.assertEqual(e.sp0,BASE+0x1b000-0xd0)

    def test_changed_return_prefix_prologue_or_c_symbol_is_rejected(self):
        for offset in (0x1100+0x32c,0x1c00,0x1c04,0x1c08):
            bad=synthetic_kernel();struct.pack_into('<I',bad,offset,0xd503201f)
            with self.subTest(offset=offset),self.assertRaises(ValueError):synthetic_pack(bad)
        bad=synthetic_kernel()
        # The ninth symbol is the private C entry; a disagreement is fatal.
        struct.pack_into('<Q',bad,0x5100+8*16+8,BASE+0x1c04)
        with self.assertRaisesRegex(ValueError,'link-register'):synthetic_pack(bad)

    def test_loader_rejects_bad_stack_or_instructions_before_writing(self):
        payload,_=synthetic_pack();h=HEADER.unpack_from(payload);raw=h[7]
        ext=HEADER.size+h[2]*40
        bads=[]
        for offset,fmt,value in ((ext+24,'<Q',BASE+0x10000),  # overlaps tables
                                 (ext+24,'<Q',BASE+0x4000), # frame outside data
                                 (raw+0x1c04,'<I',0xd503201f),
                                 (raw+0x1c00,'<I',0xd503201f)):
            bad=bytearray(payload);struct.pack_into(fmt,bad,offset,value);bads.append(bad)
        bad=bytearray(payload)
        struct.pack_into('<Q',bad,ext+24,BASE+0x4800)
        bad[raw+0x47a0]=1
        bads.append(bad)
        for bad in bads:
            ctypes.memset(self.address,0xaa,0x30000)
            self.assertEqual(self.load(bad)[0],0)
            self.assertEqual(ctypes.string_at(self.address,0x30000),b'\xaa'*0x30000)

    def test_all_highva_capture_fields_and_untranslated_pc_or_argument_reject(self):
        e=mmutests.expected_at(0x84000000);e.mode=3;e.sp0-=0xd0
        high_pc=e.virt+e.pc-e.phys
        stack=e.phys+e.sp0+0x70-e.virt
        values=[high_pc+4,0x3c4,0x3454593d,e.args,e.virt,e.phys,e.memory,e.pages,0,
                e.tcr,e.pages,e.pages+4096,e.mair,e.sp0,e.sp1,e.vbar,
                e.args&~4095,(e.pc+4)&~4095,e.virt+e.args-e.phys,0,0x300000,0,
                high_pc-8,stack&~4095]
        api=self.lib.xnu_capture_valid
        api.argtypes=[ctypes.POINTER(ctypes.c_uint64),ctypes.POINTER(Expected)]
        def valid(v):return api((ctypes.c_uint64*24)(*v),ctypes.byref(e))
        self.assertEqual(valid(values),1)
        for i in range(24):
            bad=values.copy();bad[i]^=1
            with self.subTest(register=i):self.assertEqual(valid(bad),0)
        for i,value in ((0,e.pc+4),(18,e.args),(13,e.sp0+0xd0),(23,(stack&~4095)+4096)):
            bad=values.copy();bad[i]=value
            self.assertEqual(valid(bad),0)
        self.lib.xnu_tables_valid.argtypes=[ctypes.POINTER(ctypes.c_uint64),ctypes.POINTER(Expected)]
        self.assertEqual(self.lib.xnu_tables_valid(mmutests.tables(e),ctypes.byref(e)),1)

    def test_reader_requires_new_unique_fields_in_addition_to_mmu(self):
        keys={'entry EL':2,'current EL':1,'stage':3,'handoff EL':2,
              'XNU copied':1,'XNU checkpoint':1,'XNU boot_args verified':1,'XNU current EL':1,
              'XNU MMU enabled':1,'XNU tables verified':1,'XNU translations verified':1,
              'XNU high VA verified':1,'XNU stack read verified':1,'XNU C entry verified':1}
        def record(items):
            text=('Trogdor-ipadOS native phase-0 probe\n'+''.join(f'{k}=0x{v:016x}\n' for k,v in items)).encode()
            return text+bytes(8192-len(text))
        self.assertEqual(read_record(record(keys.items()),xnu_highva=True)[1],keys)
        for key in keys:
            bad=dict(keys);del bad[key]
            with self.subTest(key=key),self.assertRaises(ValueError):read_record(record(bad.items()),xnu_highva=True)
        for key in ('XNU high VA verified','XNU stack read verified','XNU C entry verified'):
            with self.assertRaises(ValueError):read_record(record([*keys.items(),(key,1)]),xnu_highva=True)

    def test_exact_source_kernel_highva_and_prior_modes_if_available(self):
        path=ROOT/'build/xnu-source/coachz/ipados-17.7.11-21H461/coachz/tree/build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837.unstripped.noctf'
        if not path.exists():self.skipTest('optional pinned source build absent')
        data=path.read_bytes();payload,report=pack(data,adt(),highva=True)
        self.assertEqual(report['kernel_sha256'],'6b406b8cd4ee5a6c11793256d8cadc5453365e8e8edb5f487953e02e259c7e9b')
        good,_,e=self.load(payload);self.assertEqual(good,1)
        self.assertEqual(e.mode,3)
        h=HEADER.unpack_from(payload);raw=h[7]
        diffs=[i for i,(a,b) in enumerate(zip(data,payload[raw:raw+len(data)])) if a!=b]
        self.assertTrue(diffs)
        self.assertTrue(all(report['patch_file_offset']<=i<report['patch_file_offset']+8 for i in diffs))
        for kwargs in ({},{'mmu':True}):self.assertEqual(self.load(pack(data,adt(),**kwargs)[0])[0],1)

    def test_crosscompiled_highva_payload_preserves_linux_and_scalar_contract(self):
        payload,_=synthetic_pack()
        linux=struct.pack('<II6QII',0x91005a4d,0x14000001,0,4096,0xa,0,0,0,0x644d5241,0)+bytes(64)
        with tempfile.TemporaryDirectory() as tmp:
            image,report=compile_wrapper(Path(tmp),linux,4096,payload)
            self.assertEqual(report['xnu_checkpoint_version'],3)
            self.assertEqual(image[report['linux_offset']:],linux)
            self.assertLessEqual(report['header']['memory_size'],80*1024*1024)
