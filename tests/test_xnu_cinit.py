import ctypes
import hashlib
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
from unittest.mock import patch

import test_xnu_checkpoint as entrytests
import test_xnu_highva as highvatests
import test_xnu_mmu as mmutests
from test_xnu_checkpoint import ROOT, BASE, ARENA, Expected, adt
from xnu_checkpoint import pack, HEADER, HVC
from xnu_cinit import METADATA, CHECKPOINT_OFFSET, audit_metadata
from xnu_macho import macho
from linux_log import compile_wrapper
from read_probe import read_record

SOURCE = ROOT/'build/xnu-source/coachz/ipados-17.7.11-21H461/coachz/tree/build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837.unstripped.noctf'


def synthetic_payload():
    data=highvatests.synthetic_kernel()
    # The fake C body is never executed; it exercises only the format/loader.
    metadata=METADATA.pack(*(BASE+i for i in (0x1c00,0x4400,0x4900,0x4910,0x4920,0x4930)))
    with patch('xnu_checkpoint.MMU_PREFIX_SHA256',hashlib.sha256(data[0x1100:0x1400]).hexdigest()), \
         patch('xnu_checkpoint.HIGHVA_PREFIX_SHA256',hashlib.sha256(data[0x1100:0x1430]).hexdigest()), \
         patch('xnu_cinit.audit_metadata',return_value=metadata):
        return pack(data,adt(),cinit=True),data


def capture(e):
    high_pc=e.virt+e.pc-e.phys
    return [high_pc+4,0x3c4,0x3454593d,e.const_args,e.virt,e.phys,e.memory,e.pages,0,
            e.tcr,e.pages,e.pages+4096,e.mair,e.sp0,e.sp1,e.vbar,
            e.args&~4095,(e.pc+4)&~4095,e.const_args,e.virt+e.args-e.phys,0x300000,0,
            high_pc-8,(e.phys+e.sp0+0x70-e.virt)&~4095,e.sp0+0xc0,64,ARENA//64]


class XnuCinitTests(unittest.TestCase):
    setUpClass=classmethod(entrytests.XnuCheckpointTests.setUpClass.__func__)
    tearDownClass=classmethod(entrytests.XnuCheckpointTests.tearDownClass.__func__)
    load=entrytests.XnuCheckpointTests.load

    def test_one_word_patch_leaves_original_prologue_and_prior_checkpoints(self):
        (payload,report),data=synthetic_payload()
        raw=HEADER.unpack_from(payload)[7]
        modified=payload[raw:raw+len(data)]
        differences=[i for i,(a,b) in enumerate(zip(data,modified)) if a!=b]
        self.assertTrue(differences)
        self.assertTrue(all(0x2194<=i<0x2198 for i in differences))
        self.assertEqual(report['patch_bytes'],4)
        self.assertEqual(struct.unpack_from('<I',modified,0x2194)[0],HVC)
        self.assertEqual(modified[0x1c00:0x1c0c],data[0x1c00:0x1c0c])
        good,_,e=self.load(payload)
        self.assertEqual(good,1)
        self.assertEqual(e.mode,4)
        self.assertEqual(e.c_entry+CHECKPOINT_OFFSET,e.virt+e.pc-e.phys)
        self.assertEqual(e.const_args,BASE+0x4400)

    def test_unaudited_source_is_rejected(self):
        data=highvatests.synthetic_kernel()
        with self.assertRaisesRegex(ValueError,'exact independently audited'):
            audit_metadata(data,macho(data))

    def test_ranges_overlap_nonzero_and_prologue_reject_before_any_write(self):
        (payload,_),_=synthetic_payload()
        h=HEADER.unpack_from(payload);ext=HEADER.size+h[2]*40;raw=h[7]
        bads=[]
        # Metadata: C entry, globals, guard. All must belong to the proper segment.
        for offset,value in ((ext+48,BASE+0x1c04),(ext+56,BASE+0x2000),
                             (ext+56,BASE+0x1b000-0x200), # stack overlap
                             (ext+56,BASE+0x8000),       # table overlap
                             (ext+64,BASE+0x4400),       # globals overlap
                             (ext+72,BASE+0x20000),      # out of kernel
                             (ext+80,BASE+0x4930),       # guard overlap
                             (ext+88,BASE+0x4931),       # unaligned guard
                             (ext+24,BASE+0x4080)):
            bad=bytearray(payload);struct.pack_into('<Q',bad,offset,value);bads.append(bad)
        for offset in (0x4400,0x487f,0x4900,0x4910,0x4920,0x4930,0x1c00,0x1c04,0x2194):
            bad=bytearray(payload);bad[raw+offset]^=1;bads.append(bad)
        for bad in bads:
            ctypes.memset(self.address,0xaa,0x30000)
            self.assertEqual(self.load(bad)[0],0)
            self.assertEqual(ctypes.string_at(self.address,0x30000),b'\xaa'*0x30000)

    def test_all_registers_and_cache_loop_counts_reject_individually(self):
        e=mmutests.expected_at(0x84000000);e.mode=4;e.sp0-=0xd0
        e.const_args=BASE+0x4400;e.c_entry=e.virt+e.pc-e.phys-CHECKPOINT_OFFSET
        values=capture(e)
        api=self.lib.xnu_capture_valid
        api.argtypes=[ctypes.POINTER(ctypes.c_uint64),ctypes.POINTER(Expected)]
        def valid(v):return api((ctypes.c_uint64*27)(*v),ctypes.byref(e))
        self.assertEqual(valid(values),1)
        for index in range(27):
            bad=values.copy();bad[index]^=1
            with self.subTest(register=index):self.assertEqual(valid(bad),0)
        for index,value in ((3,e.args),(18,e.virt+e.args-e.phys),(19,0),(25,0),(25,3),
                            (25,262144),(26,0),(0,e.pc+4)):
            bad=values.copy();bad[index]=value
            with self.subTest(register=index,value=value):self.assertEqual(valid(bad),0)
        self.lib.xnu_tables_valid.argtypes=[ctypes.POINTER(ctypes.c_uint64),ctypes.POINTER(Expected)]
        self.assertEqual(self.lib.xnu_tables_valid(mmutests.tables(e),ctypes.byref(e)),1)

    def populate_c_results(self,e):
        def addr(va):return e.phys+va-e.virt
        ctypes.memmove(addr(e.const_args),e.args,0x480)
        ctypes.c_uint64.from_address(addr(e.boot_args)).value=e.const_args
        for offset,value in {0x68:0,0x90:e.memory,0x98:e.phys,0xa0:e.virt,0xa8:0,
                             0xb0:e.args,0xb8:e.c_entry,0xc8:e.c_entry,0x88:e.pages,-16:e.sp0+0xc0,
                             -8:e.virt+e.pc-e.phys-8}.items():
            ctypes.c_uint64.from_address(addr(e.sp0+offset)).value=value

    def test_entire_copy_globals_and_native_stack_frames_reject_corruption(self):
        (payload,_),_=synthetic_payload()
        good,_,e=self.load(payload);self.assertEqual(good,1)
        api=self.lib.xnu_cinit_valid;api.argtypes=[ctypes.POINTER(Expected)]
        self.assertEqual(api(ctypes.byref(e)),0) # execution has not happened
        self.populate_c_results(e)
        self.assertEqual(api(ctypes.byref(e)),1)
        offsets=[e.const_args+i for i in range(0x480)]
        offsets += [e.boot_args,e.slide,e.static_cache,e.guard]
        offsets += [e.sp0+i for i in range(-16,0xd0)]
        for va in offsets:
            byte=ctypes.c_uint8.from_address(e.phys+va-e.virt);old=byte.value;byte.value^=1
            self.assertEqual(api(ctypes.byref(e)),0)
            byte.value=old
        for field,value in (('const_args',BASE-8),('boot_args',BASE+ARENA),('guard',BASE+1),
                            ('sp0',0),('end',e.phys-1),('args',e.end)):
            old=getattr(e,field);setattr(e,field,value)
            with self.subTest(field=field):self.assertEqual(api(ctypes.byref(e)),0)
            setattr(e,field,old)

    def test_reader_requires_all_fifteen_unique_success_fields(self):
        keys={'entry EL':2,'current EL':1,'stage':3,'handoff EL':2,
              'XNU copied':1,'XNU checkpoint':1,'XNU boot_args verified':1,'XNU current EL':1,
              'XNU MMU enabled':1,'XNU tables verified':1,'XNU translations verified':1,
              'XNU C body verified':1,'XNU stack writes verified':1,'XNU args copy verified':1,
              'XNU cache maintenance verified':1}
        def record(items):
            data=('Trogdor-ipadOS native phase-0 probe\n'+''.join(f'{k}=0x{v:016x}\n' for k,v in items)).encode()
            return data+bytes(8192-len(data))
        self.assertEqual(read_record(record(keys.items()),xnu_cinit=True)[1],keys)
        for key in keys:
            for kind in ('missing','zero','duplicate'):
                items=[(k,v) for k,v in keys.items() if k!=key]
                if kind=='zero':items.append((key,0))
                if kind=='duplicate':items.extend([(key,keys[key])]*2)
                with self.subTest(key=key,kind=kind),self.assertRaises(ValueError):
                    read_record(record(items),xnu_cinit=True)

    def test_exact_source_input_and_all_prior_modes_if_available(self):
        if not SOURCE.exists():self.skipTest('optional pinned source build absent')
        data=SOURCE.read_bytes();payload,report=pack(data,adt(),cinit=True)
        good,_,e=self.load(payload);self.assertEqual(good,1)
        self.assertEqual(e.mode,4)
        self.assertEqual(e.c_entry,0xfffffff0079b3704)
        self.assertEqual(e.const_args,0xfffffff00716acf0)
        self.assertEqual(e.boot_args,0xfffffff00716b170)
        raw=HEADER.unpack_from(payload)[7]
        diffs=[i for i,(a,b) in enumerate(zip(data,payload[raw:raw+len(data)])) if a!=b]
        self.assertTrue(diffs)
        self.assertTrue(all(report['patch_file_offset']<=i<report['patch_file_offset']+4 for i in diffs))
        self.populate_c_results(e)
        self.lib.xnu_cinit_valid.argtypes=[ctypes.POINTER(Expected)]
        self.assertEqual(self.lib.xnu_cinit_valid(ctypes.byref(e)),1)
        bad=bytearray(data);bad[-1]^=1
        with self.assertRaisesRegex(ValueError,'exact independently audited'):pack(bad,adt(),cinit=True)
        for kwargs in ({},{'mmu':True},{'highva':True}):
            self.assertEqual(self.load(pack(data,adt(),**kwargs)[0])[0],1)

    def test_crosscompiled_cinit_payload_and_generated_cache_maintenance(self):
        (payload,_),_=synthetic_payload()
        linux=struct.pack('<II6QII',0x91005a4d,0x14000001,0,4096,0xa,0,0,0,0x644d5241,0)+bytes(64)
        with tempfile.TemporaryDirectory() as tmp:
            image,report=compile_wrapper(Path(tmp),linux,4096,payload)
            self.assertEqual(report['xnu_checkpoint_version'],4)
            self.assertEqual(image[report['linux_offset']:],linux)
            self.assertLessEqual(report['header']['memory_size'],80*1024*1024)
            disasm=subprocess.check_output(['xcrun','llvm-objdump','-d',str(Path(tmp)/'probe.elf')],text=True)
            body=disasm.split('<xnu_clean_arena>:',1)[1].split('<xnu_prepare>:',1)[0]
            self.assertIn('CTR_EL0',body)
            loop=body.split('<xnu_clean_line>:',1)[1]
            self.assertLess(loop.index('dc\tcivac'),loop.index('msr\tSCTLR_EL1'))
            self.assertIn('dsb\tsy',loop)
