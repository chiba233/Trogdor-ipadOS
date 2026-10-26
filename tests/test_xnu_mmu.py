import ctypes
import hashlib
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
from unittest.mock import patch

import test_xnu_checkpoint as entrytests
from test_xnu_checkpoint import ROOT, BASE, ARENA, Expected, adt, fixture
from xnu_checkpoint import pack, HEADER, MMU_OFFSET
from xnu_macho import macho, backed
from linux_log import compile_wrapper
from read_probe import read_record


def synthetic_kernel():
    data=fixture();at=32
    while at<32+struct.unpack_from('<I',data,20)[0]:
        cmd,size=struct.unpack_from('<II',data,at)
        if cmd==25:
            name=data[at+8:at+24].split(b'\0')[0]
            if name==b'__DATA':struct.pack_into('<Q',data,at+32,0x18000)
            if name==b'__LINKEDIT':struct.pack_into('<Q',data,at+24,BASE+0x1c000)
        if cmd==2:symcommand=at
        at+=size
    names={'_start_first_cpu':(BASE+0x1100,1),'_bootstrap_pagetables':(BASE+0x8000,3),
           '_set_mmu_ttb':(BASE+0x1800,1),'__mh_execute_header':(BASE,1),
           '_sysreg_restore':(BASE+0x4010,2),'_ExceptionVectorsBase':(BASE+0x2000,1),
           '_intstack_top':(BASE+0xa000,3),'_excepstack_top':(BASE+0xb000,3)}
    strings=b'\0'
    for i,(name,(value,section)) in enumerate(names.items()):
        struct.pack_into('<IBBHQ',data,0x5100+i*16,len(strings),0xf,section,0,value)
        strings+=name.encode()+b'\0'
    struct.pack_into('<4I',data,symcommand+8,0x5100,len(names),0x5200,len(strings))
    data[0x5200:0x5200+len(strings)]=strings
    data[0x1800:0x1810]=bytes.fromhex('9f3b03d5002018d5df3f03d5c0035fd6')
    struct.pack_into('<Q',data,0x4010,0x22a51a251a)
    struct.pack_into('<3I',data,0x1100+MMU_OFFSET-8,0xd5181000,0xd5033fdf,0xf2e00001)
    return data


def synthetic_payload():
    data=synthetic_kernel()
    # Synthetic code is never executed; only this test substitutes its prefix hash.
    digest=hashlib.sha256(data[0x1100:0x1100+MMU_OFFSET+4]).hexdigest()
    with patch('xnu_checkpoint.MMU_PREFIX_SHA256',digest):
        return pack(data,adt(),mmu=True)


def expected_at(physical):
    e=Expected()
    for key,value in dict(mode=2,virt=BASE,phys=physical+0x4000,memory=ARENA-0x4000,
        pages=physical+0x10000,pc=physical+0x80000,args=physical+0x180000,
        tcr=0x22a51a251a,mair=0x0c0804ff00bb44ff,vbar=BASE+0x40000,
        sp0=BASE+0x100000,sp1=BASE+0x110000,end=physical+0x190000).items():
        setattr(e,key,value)
    return e


def tables(e):
    pool=(ctypes.c_uint64*8192)();page=2
    for root,base in enumerate((e.phys,e.virt)):
        groups={}
        for i in range(e.memory//0x200000):
            va=base+i*0x200000
            group=(va>>30)&255
            groups.setdefault(group,[]).append(((va>>21)&511,(e.phys&~0x1fffff)+i*0x200000))
        for group,entries in groups.items():
            pool[root*512+group]=(e.pages+page*4096)|3
            for slot,pa in entries:pool[page*512+slot]=pa|0x601
            page+=1
    return pool


class XnuMmuTests(unittest.TestCase):
    setUpClass=classmethod(entrytests.XnuCheckpointTests.setUpClass.__func__)
    tearDownClass=classmethod(entrytests.XnuCheckpointTests.tearDownClass.__func__)
    load=entrytests.XnuCheckpointTests.load

    def test_mmu_payload_is_separate_and_preserves_old_checkpoint_instruction(self):
        payload,report=synthetic_payload();data=synthetic_kernel()
        h=HEADER.unpack_from(payload)
        self.assertEqual(h[1],2)
        raw=h[7];instrumented=payload[raw:raw+len(data)]
        changed=[i for i,(a,b) in enumerate(zip(data,instrumented)) if a!=b]
        self.assertTrue(changed)
        self.assertTrue(all(0x1100+MMU_OFFSET<=i<0x1100+MMU_OFFSET+4 for i in changed))
        self.assertEqual(instrumented[0x1130:0x1134],data[0x1130:0x1134])
        self.assertEqual(report['kernel_alignment_bias'],0x4000)
        with self.assertRaisesRegex(ValueError,'instruction audit'):
            pack(data,adt(),mmu=True)

    def test_shifted_loader_keeps_block_offsets_and_aligned_boot_memory_base(self):
        payload,_=synthetic_payload();good,out,e=self.load(payload)
        self.assertEqual(good,1)
        self.assertEqual(e.phys,self.address+0x4000)
        self.assertEqual(e.phys%0x200000,BASE%0x200000)
        self.assertEqual(out.entry,self.address+0x5000)
        self.assertEqual(ctypes.c_uint64.from_address(self.address+0x8000).value,BASE+0x1100)
        self.assertEqual(struct.unpack('<3Q',ctypes.string_at(out.args+8,24)),
                         (BASE-0x4000,self.address,ARENA))
        self.assertEqual(e.memory,ARENA-0x4000)
        self.assertLessEqual(e.end,self.address+22*1024*1024)

    def test_mmu_metadata_and_pool_reject_before_writes(self):
        payload,_=synthetic_payload();count=HEADER.unpack_from(payload)[2]
        ext=HEADER.size+count*40
        bads=[]
        for offset,value in ((ext,0),(ext+8,0),(ext+16,BASE-16),(ext+24,BASE+1),
                              (ext+40,4096),(16+3*8,BASE+0x19000)):
            bad=bytearray(payload);struct.pack_into('<Q',bad,offset,value);bads.append(bad)
        for bad in bads:
            ctypes.memset(self.address,0xaa,0x30000)
            self.assertEqual(self.load(bad)[0],0)
            self.assertEqual(ctypes.string_at(self.address,0x30000),b'\xaa'*0x30000)

    def test_each_mmu_register_and_par_failure_is_rejected(self):
        e=expected_at(0x84000000)
        values=[e.pc+4,0x3c4,0x3454593d,e.args,e.virt,e.phys,e.memory,e.pages,0,
                e.tcr,e.pages,e.pages+4096,e.mair,e.sp0,e.sp1,e.vbar,
                e.args&~4095,(e.pc+4)&~4095]
        api=self.lib.xnu_capture_valid
        api.argtypes=[ctypes.POINTER(ctypes.c_uint64),ctypes.POINTER(Expected)]
        def valid(v):return api((ctypes.c_uint64*18)(*v),ctypes.byref(e))
        self.assertEqual(valid(values),1)
        for i in range(18):
            bad=values.copy();bad[i]^=1
            with self.subTest(register=i):self.assertEqual(valid(bad),0)
        for bit in (1,4,0x1000):
            bad=values.copy();bad[2]&=~bit
            self.assertEqual(valid(bad),0)
        for i in (16,17):
            bad=values.copy();bad[i]^=4096
            self.assertEqual(valid(bad),0)

    def test_full_table_pool_checks_identity_high_aliases_and_unused_words(self):
        api=self.lib.xnu_tables_valid
        api.argtypes=[ctypes.POINTER(ctypes.c_uint64),ctypes.POINTER(Expected)]
        for physical in (0x84000000,0xbfe00000):
            e=expected_at(physical);pool=tables(e)
            self.assertEqual(api(pool,ctypes.byref(e)),1)
            populated=[i for i in range(8192) if pool[i]]
            for i in populated+[8191]:
                old=pool[i];pool[i]^=1
                with self.subTest(physical=physical,word=i):self.assertEqual(api(pool,ctypes.byref(e)),0)
                pool[i]=old
            e.phys+=4096
            self.assertEqual(api(pool,ctypes.byref(e)),0)

    def test_mmu_reader_requires_distinct_translation_and_table_evidence(self):
        keys={'entry EL':2,'current EL':1,'stage':3,'handoff EL':2,
              'XNU copied':1,'XNU checkpoint':1,'XNU boot_args verified':1,'XNU current EL':1,
              'XNU MMU enabled':1,'XNU tables verified':1,'XNU translations verified':1}
        def record(d):
            t=('Trogdor-ipadOS native phase-0 probe\n'+''.join(f'{k}=0x{v:016x}\n' for k,v in d.items())).encode()
            return t+bytes(8192-len(t))
        self.assertEqual(read_record(record(keys),xnu_mmu=True)[1],keys)
        for key in ('XNU MMU enabled','XNU tables verified','XNU translations verified'):
            bad=dict(keys);bad[key]=0
            with self.assertRaises(ValueError):read_record(record(bad),xnu_mmu=True)

    def test_real_source_guard_and_old_new_payloads_if_available(self):
        path=ROOT/'build/xnu-source/coachz/ipados-17.7.11-21H461/coachz/tree/build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837.unstripped.noctf'
        if not path.exists():self.skipTest('optional pinned source build absent')
        data=path.read_bytes();payload,report=pack(data,adt(),mmu=True)
        self.assertEqual(report['kernel_sha256'],'6b406b8cd4ee5a6c11793256d8cadc5453365e8e8edb5f487953e02e259c7e9b')
        good,_,e=self.load(payload)
        self.assertEqual(good,1)
        self.assertEqual(e.mode,2)
        raw=HEADER.unpack_from(payload)[7]
        modified=payload[raw:raw+len(data)]
        changed=[i for i,(a,b) in enumerate(zip(data,modified)) if a!=b]
        self.assertTrue(all(report['patch_file_offset']<=i<report['patch_file_offset']+4 for i in changed))
        old,_=pack(data,adt())
        self.assertEqual(self.load(old)[0],1)

    def test_crosscompiled_mmu_wrapper_keeps_74mib_budget(self):
        payload,_=synthetic_payload()
        linux=struct.pack('<II6QII',0x91005a4d,0x14000001,0,4096,0xa,0,0,0,0x644d5241,0)+bytes(64)
        with tempfile.TemporaryDirectory() as tmp:
            image,report=compile_wrapper(Path(tmp),linux,4096,payload)
            self.assertLessEqual(report['header']['memory_size'],80*1024*1024)
            self.assertEqual(image[report['linux_offset']:],linux)
