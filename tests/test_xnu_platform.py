import ctypes
import hashlib
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch

import test_xnu_checkpoint as entrytests
import test_xnu_cinit as cinittests
import test_xnu_cpu as cputests
from test_xnu_checkpoint import ROOT,BASE,Expected,adt_node
from xnu_checkpoint import pack,HEADER,HVC
from xnu_platform import search_offsets,audit_metadata
from xnu_macho import macho
from linux_log import compile_wrapper
from read_probe import read_record


def adt(extra=()):
    return adt_node('device-tree',children=[
        adt_node('chosen',{'dram-base':bytes(8),'dram-size':bytes(8)}),
        adt_node('cpus',children=[adt_node('cpu0',{'reg':bytes(4),'timebase-frequency':struct.pack('<I',19200000)})]),
        adt_node('arm-io'),*extra])


def synthetic_payload():
    (prior,_),data=cputests.synthetic_payload()
    at=HEADER.size+HEADER.unpack_from(prior)[2]*40;tree=adt();node,cursor=search_offsets(tree)
    meta=struct.pack('<9Q',*(BASE+i for i in (0x5600,0x56b0,0x5700,0x5720,0x2500,0x2600,0x2400)),node,cursor)
    with patch('xnu_checkpoint.MMU_PREFIX_SHA256',hashlib.sha256(data[0x1100:0x1400]).hexdigest()), \
         patch('xnu_checkpoint.HIGHVA_PREFIX_SHA256',hashlib.sha256(data[0x1100:0x1430]).hexdigest()), \
         patch('xnu_cinit.audit_metadata',return_value=prior[at+48:at+96]), \
         patch('xnu_cpu.audit_metadata',return_value=prior[at+96:at+112]), \
         patch('xnu_platform.audit_metadata',return_value=(meta,BASE+0x2434)):
        return pack(data,tree,platform=True),data


def capture(e):
    c=cputests.capture(e)
    c[3]=0;c[18]=1;c[19]=e.const_args;c[24]=e.sp0+0x30
    return c


class XnuPlatformTests(unittest.TestCase):
    setUpClass=classmethod(entrytests.XnuCheckpointTests.setUpClass.__func__)
    tearDownClass=classmethod(entrytests.XnuCheckpointTests.tearDownClass.__func__)
    load=entrytests.XnuCheckpointTests.load

    def test_independent_adt_search_offset_and_limits(self):
        tree=adt();node,cursor=search_offsets(tree)
        self.assertEqual(cursor,node+52)
        self.assertEqual(tree[node+44:cursor],b'arm-io\0\0')
        bad=bytearray(tree);bad[8:40]=b'x'*32
        with self.assertRaises(ValueError):search_offsets(bad)
        bad=bytearray(tree);bad[node+50]=ord('x')
        with self.assertRaises(ValueError):search_offsets(bad)
        for offset in (13,51):
            bad=bytearray(tree);bad[node+offset]=1
            with self.assertRaisesRegex(ValueError,'canonical'):search_offsets(bad)
        with self.assertRaises(ValueError):search_offsets(adt([adt_node('arm-io')]))
        deep=adt_node('last')
        for i in range(5):deep=adt_node('level'+str(i),children=[deep])
        with self.assertRaisesRegex(ValueError,'recursion/node'):search_offsets(adt([deep]))
        with self.assertRaisesRegex(ValueError,'recursion/node'):
            search_offsets(adt([adt_node('n'+str(i)) for i in range(65)]))
        with self.assertRaisesRegex(ValueError,'search size'):
            search_offsets(adt([adt_node('large',{'data':bytes(65536)})]))

    def test_patch_is_in_soc_helper_and_cpu_seed_is_removed(self):
        (payload,report),data=synthetic_payload();h=HEADER.unpack_from(payload);raw=h[7]
        differences=[i for i,(a,b) in enumerate(zip(data,payload[raw:raw+len(data)])) if a!=b]
        self.assertTrue(differences)
        self.assertTrue(all(0x2434<=i<0x2438 for i in differences))
        self.assertEqual(h[1],6);self.assertTrue(report['platform_checkpoint'])
        self.assertEqual(struct.unpack_from('<I',payload,raw+0x2434)[0],HVC)
        good,_,e=self.load(payload);self.assertEqual(good,1)
        self.assertEqual(e.sp0,BASE+0x1b000-0x200)
        self.assertEqual(ctypes.string_at(e.phys+e.cpu_data-e.virt,0x500),bytes(0x500))
        self.assertEqual(e.soc+0x34,e.virt+e.pc-e.phys)

    def test_new_metadata_globals_tree_and_stack_reject_before_writes(self):
        (payload,_),_=synthetic_payload();h=HEADER.unpack_from(payload);ext=HEADER.size+h[2]*40;raw=h[7];tree=h[9]
        bads=[]
        for offset,value in ((ext+112,BASE+0x2000),(ext+112,BASE+0x4400),
                             (ext+120,BASE+0x5600),(ext+128,BASE+0x8000),
                             (ext+136,BASE+0x1aff0),(ext+144,0),(ext+152,BASE+0x30000),
                             (ext+160,BASE+0x2404),(ext+168,2**64-4),
                             (ext+176,0),(ext+24,BASE+0x18500)):
            b=bytearray(payload);struct.pack_into('<Q',b,offset,value);bads.append(b)
        for offset in (40,44,50):
            b=bytearray(payload);node=struct.unpack_from('<Q',b,ext+168)[0]
            b[tree+node+offset]^=1;bads.append(b)
        # A file-backed, otherwise disjoint PE region must start with zero bytes.
        b=bytearray(payload);struct.pack_into('<Q',b,ext+112,BASE+0x4a00);b[raw+0x4a00]=1;bads.append(b)
        for i,bad in enumerate(bads):
            with self.subTest(case=i):
                ctypes.memset(self.address,0xaa,0x30000)
                self.assertEqual(self.load(bad)[0],0)
                self.assertEqual(ctypes.string_at(self.address,0x30000),b'\xaa'*0x30000)

    def populate(self,e):
        def addr(va):return e.phys+va-e.virt
        def write(va,b):ctypes.memmove(addr(va),bytes(b),len(b))
        # Reuse the previously audited parent C contract, then apply its native
        # call-frame overwrite by the active PE function.
        parent=Expected.from_buffer_copy(e);parent.mode=4;parent.sp0=e.sp0+0x130
        parent.pc=e.phys+e.c_entry+0x594-e.virt
        cinittests.XnuCinitTests.populate_c_results(self,parent)
        state=bytearray(0xb0);struct.pack_into('<I',state,0,1);state[0x89]=1
        state[0x38:0x38+25]=b'BBBBBBBBGGGGGGGGRRRRRRRR\0'
        struct.pack_into('<3Q',state,0x98,e.tree,e.const_args,e.tree_size);write(e.pe_state,state)
        write(e.dt_root,struct.pack('<3QI',e.tree,e.tree_size,e.tree+e.tree_size,1))
        write(e.dt_cursor,struct.pack('<Q',e.tree+e.cursor))
        cpu=bytearray(0x500)
        for at,fmt,value in ((0x40,'<I',0x80000000),(0x100,'<Q',2**64-1),
                             (0x150,'<Q',e.rtclock),(0x1cc,'<I',0x50535553),(0x280,'<I',2**32-1)):
            struct.pack_into(fmt,cpu,at,value)
        write(e.cpu_data,cpu)
        soc=bytearray(0x40);struct.pack_into('<Q',soc,0x18,e.tree+e.node)
        struct.pack_into('<3Q',soc,0x28,e.const_args,e.sp0+0xe0,e.identify+0x24);write(e.sp0,soc)
        identify=bytearray(0xb0)
        for at,value in ((0x68,e.pages),(0x70,e.memory),(0x78,e.phys),(0x80,e.virt),
                         (0x98,e.const_args),(0xa0,e.sp0+0x120),(0xa8,e.pe_init+0xf4)):
            struct.pack_into('<Q',identify,at,value)
        write(e.sp0+0x40,identify)
        pe=bytearray(0x40);struct.pack_into('<4Q',pe,0x20,e.const_args,e.cpu_data,e.sp0+0x1f0,e.c_entry+0x5b0)
        write(e.sp0+0xf0,pe)

    def valid(self,e):
        for name in ('xnu_platform_valid','xnu_cpu_valid','xnu_cinit_valid'):
            api=getattr(self.lib,name);api.argtypes=[ctypes.POINTER(Expected)]
            if not api(ctypes.byref(e)):return 0
        return 1

    def test_all_globals_active_frames_and_guard_reject_each_byte_corruption(self):
        (payload,_),_=synthetic_payload();good,_,e=self.load(payload);self.assertEqual(good,1)
        self.assertEqual(self.valid(e),0);self.populate(e);self.assertEqual(self.valid(e),1)
        regions=((e.pe_state,0xb0),(e.protection,4),(e.dt_root,28),(e.dt_cursor,8),
                 (e.sp0,0x200),(e.sp0-0xe00,64))
        for va,n in regions:
            for off in range(n):
                byte=ctypes.c_uint8.from_address(e.phys+va+off-e.virt);old=byte.value;byte.value^=1
                with self.subTest(va=va,byte=off):self.assertEqual(self.valid(e),0)
                byte.value=old
        # Retired traversal frames may contain valid call residue, outside the active frames.
        ctypes.c_uint64.from_address(e.phys+e.sp0-0x60-e.virt).value=0x123456
        self.assertEqual(self.valid(e),1)
        ctypes.c_uint64.from_address(e.phys+e.sp0+0x128-e.virt).value=e.c_entry+0x58c
        self.assertEqual(self.valid(e),0) # previous memmove LR no longer fits the active PE frame

    def test_platform_validator_bad_ranges_and_cpu_canary_reject(self):
        (payload,_),_=synthetic_payload();good,_,e=self.load(payload);self.assertEqual(good,1)
        self.populate(e);self.assertEqual(self.valid(e),1)
        for key,value in (('sp0',0),('pe_state',BASE-8),('dt_root',BASE+1),('dt_cursor',BASE+0x2000000),
                          ('tree_size',0),('node',2**64-4),('cursor',0),('end',e.phys-1)):
            old=getattr(e,key);setattr(e,key,value)
            with self.subTest(field=key):self.assertEqual(self.valid(e),0)
            setattr(e,key,old)
        ctypes.c_uint8.from_address(e.phys+e.cpu_data-e.virt).value=0xa5
        self.assertEqual(self.valid(e),0)

    def test_platform_register_capture_rejects_every_field_and_prior_mode_values(self):
        (payload,_),_=synthetic_payload();good,_,e=self.load(payload);self.assertEqual(good,1)
        api=self.lib.xnu_capture_valid;api.argtypes=[ctypes.POINTER(ctypes.c_uint64),ctypes.POINTER(Expected)]
        def valid(v):return api((ctypes.c_uint64*27)(*v),ctypes.byref(e))
        values=capture(e);self.assertEqual(valid(values),1)
        for i in range(27):
            bad=values.copy();bad[i]^=1
            with self.subTest(register=i):self.assertEqual(valid(bad),0)
        for i,value in ((3,e.const_args),(18,e.cpu_data),(19,e.cpu_data),(24,e.sp0+0xc0),(2,values[2]|2)):
            bad=values.copy();bad[i]=value;self.assertEqual(valid(bad),0)

    def test_reader_requires_all_nineteen_unique_fields(self):
        keys={'entry EL':2,'current EL':1,'stage':3,'handoff EL':2,
              'XNU copied':1,'XNU checkpoint':1,'XNU boot_args verified':1,'XNU current EL':1,
              'XNU MMU enabled':1,'XNU tables verified':1,'XNU translations verified':1,
              'XNU C body verified':1,'XNU stack writes verified':1,'XNU args copy verified':1,
              'XNU cache maintenance verified':1,'XNU CPU data verified':1,
              'XNU platform state verified':1,'XNU ADT binding verified':1,'XNU ADT lookup verified':1}
        def record(items):
            b=('Trogdor-ipadOS native phase-0 probe\n'+''.join(f'{k}=0x{v:016x}\n' for k,v in items)).encode()
            return b+bytes(8192-len(b))
        self.assertEqual(read_record(record(keys.items()),xnu_platform=True)[1],keys)
        for key in keys:
            for how in ('missing','zero','duplicate'):
                items=[(k,v) for k,v in keys.items() if k!=key]
                if how=='zero':items.append((key,0))
                if how=='duplicate':items.extend([(key,keys[key])]*2)
                with self.subTest(key=key,how=how),self.assertRaises(ValueError):read_record(record(items),xnu_platform=True)

    def test_exact_source_patch_loader_and_all_prior_modes(self):
        if not cinittests.SOURCE.exists():self.skipTest('optional pinned source absent')
        data=cinittests.SOURCE.read_bytes();payload,report=pack(data,adt(),platform=True)
        good,_,e=self.load(payload);self.assertEqual(good,1)
        self.populate(e);self.assertEqual(self.valid(e),1)
        self.assertEqual(e.soc,0xfffffff0079483dc)
        self.assertEqual(e.pe_state,0xfffffff0071fc828)
        h=HEADER.unpack_from(payload);raw=h[7]
        diff=[i for i,(a,b) in enumerate(zip(data,payload[raw:raw+len(data)])) if a!=b]
        self.assertTrue(diff)
        self.assertTrue(all(report['patch_file_offset']<=i<report['patch_file_offset']+4 for i in diff))
        bad=bytearray(data);bad[report['patch_file_offset']]^=1
        with self.assertRaises(ValueError):pack(bad,adt(),platform=True)
        for kwargs in ({},{'mmu':True},{'highva':True},{'cinit':True},{'cpu_init':True}):
            self.assertEqual(self.load(pack(data,adt(),**kwargs)[0])[0],1)

    def test_source_audit_rejects_synthetic_and_crosscompile_preserves_linux(self):
        (payload,_),data=synthetic_payload()
        with self.assertRaisesRegex(ValueError,'exact audited'):audit_metadata(data,macho(data),adt())
        linux=struct.pack('<II6QII',0x91005a4d,0x14000001,0,4096,0xa,0,0,0,0x644d5241,0)+bytes(64)
        with tempfile.TemporaryDirectory() as tmp:
            image,report=compile_wrapper(Path(tmp),linux,4096,payload)
            self.assertEqual(report['xnu_checkpoint_version'],6)
            self.assertEqual(image[report['linux_offset']:],linux)
            self.assertLessEqual(report['header']['memory_size'],80*1024*1024)
