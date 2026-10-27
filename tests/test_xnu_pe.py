import ctypes
import hashlib
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch
import test_xnu_checkpoint as entrytests
import test_xnu_cinit as cinittests
import test_xnu_platform as platformtests
from test_xnu_checkpoint import BASE,ARENA,Expected,adt_node
from xnu_checkpoint import pack,HEADER,HVC
from xnu_pe import tree_contract,audit_metadata,COMMAND_LINE
from xnu_platform import search_offsets
from xnu_macho import macho
from linux_log import compile_wrapper
from read_probe import read_record


def adt(root=None,chosen=None,cpu=None,soc=None,extra=()):
    return adt_node('device-tree',{'model':b'Coachz-test\0',**(root or {})},[
        adt_node('chosen',{'dram-base':bytes(8),'dram-size':bytes(8),**(chosen or {})}),
        adt_node('cpus',children=[adt_node('cpu0',{'reg':bytes(4),'state':b'running\0',
            'timebase-frequency':struct.pack('<I',19200000),**(cpu or {})})]),
        adt_node('arm-io',{'device_type':b'test-soc\0','ranges':struct.pack('<3Q',0,0x17000000,0x1000000),**(soc or {})}),*extra])


def synthetic_payload(tree=None):
    (prior,_),data=platformtests.synthetic_payload();data=bytearray(data);data[0x4a00]=1
    at=HEADER.size+HEADER.unpack_from(prior)[2]*40
    tree=adt() if tree is None else tree
    platform=prior[at+112:at+168]+struct.pack('<2Q',*search_offsets(tree))
    meta=struct.pack('<5Q',*(BASE+i for i in (0x5800,0x5900,0x5b08,0x5b0c,0x4a00)))
    with patch('xnu_checkpoint.MMU_PREFIX_SHA256',hashlib.sha256(data[0x1100:0x1400]).hexdigest()), \
         patch('xnu_checkpoint.HIGHVA_PREFIX_SHA256',hashlib.sha256(data[0x1100:0x1430]).hexdigest()), \
         patch('xnu_cinit.audit_metadata',return_value=prior[at+48:at+96]), \
         patch('xnu_cpu.audit_metadata',return_value=prior[at+96:at+112]), \
         patch('xnu_platform.audit_metadata',return_value=(platform,BASE+0x2434)), \
         patch('xnu_pe.audit_metadata',return_value=(meta,BASE+0x21b0)):
        return pack(data,tree,pe_return=True),data


class XnuPeTests(unittest.TestCase):
    setUpClass=classmethod(entrytests.XnuCheckpointTests.setUpClass.__func__)
    tearDownClass=classmethod(entrytests.XnuCheckpointTests.tearDownClass.__func__)
    load=entrytests.XnuCheckpointTests.load

    def contract(self,data):
        api=self.lib.xnu_pe_contract;api.argtypes=[ctypes.c_void_p,ctypes.c_size_t,ctypes.POINTER(ctypes.c_uint64)]
        out=(ctypes.c_uint64*8)();good=api(ctypes.create_string_buffer(data),len(data),out)
        return tuple(out) if good else None

    def test_independent_tree_contracts_agree_and_reject_unaudited_branches(self):
        self.assertEqual(self.contract(adt()),tree_contract(adt()))
        bads=[adt(root={k:b'x\0'}) for k in ('target-type','target-is-fpga')]
        bads += [adt(chosen={k:bytes(8)}) for k in ('debug-enabled','firmware-version',
                  'system-firmware-version','unique-chip-id','dram-vendor-id')]
        bads += [adt(cpu={k:bytes(4)}) for k in ('bus-frequency','memory-frequency','peripheral-frequency','fixed-frequency','clock-frequency')]
        bads += [adt(cpu={'state':b'halted\0'}),adt(cpu={'timebase-frequency':bytes(8)}),
                 adt(cpu={'timebase-frequency':bytes(4)}),adt(root={'model':b''}),adt(root={'model':b'x'*33+b'\0'}),
                 adt(soc={'device_type':b'not-terminated'}),adt(soc={'ranges':bytes(24)}),adt(soc={'ranges':bytes(16)}),
                 adt(extra=[adt_node('chosen')]),adt(extra=[adt_node('unknown')]),
                 adt(extra=[adt_node('defaults',children=[adt_node('same'),adt_node('same')])])]
        deep=adt_node('last')
        for i in range(5):deep=adt_node('deep'+str(i),children=[deep])
        bads.append(adt(extra=[adt_node('defaults',children=[deep])]))
        for data in bads:
            with self.subTest(case=data[-24:]):
                with self.assertRaises(ValueError):tree_contract(data)
                self.assertIsNone(self.contract(data))
        # Compare independent parsers under deterministic malformed/truncated inputs.
        data=adt()
        for i in range(len(data)):
            bad=bytearray(data);bad[i]^=0x80
            try:expected=tree_contract(bad)
            except (ValueError,KeyError):expected=None
            self.assertEqual(self.contract(bytes(bad)),expected, i)
        for n in range(len(data)):
            self.assertIsNone(self.contract(data[:n]))

    def populate(self,e):
        def write(va,data):ctypes.memmove(e.phys+va-e.virt,bytes(data),len(data))
        def word(va,value):write(va,struct.pack('<Q',value))
        cinittests.XnuCinitTests.populate_c_results(self,e)
        word(e.sp0-8,e.c_entry+0x5b0)
        cpu=bytearray(0x500)
        for at,fmt,val in ((0x40,'<I',0x80000000),(0x100,'<Q',2**64-1),(0x150,'<Q',e.rtclock),
                           (0x1cc,'<I',0x50535553),(0x280,'<I',2**32-1)):
            struct.pack_into(fmt,cpu,at,val)
        write(e.cpu_data,cpu)
        state=bytearray(0xb0);state[0]=1;state[0x89]=1;state[0x38:0x51]=b'BBBBBBBBGGGGGGGGRRRRRRRR\0'
        struct.pack_into('<3Q',state,0x98,e.tree,e.const_args,e.tree_size);write(e.pe_state,state)
        write(e.dt_root,struct.pack('<3QI',e.tree,e.tree_size,e.tree+e.tree_size,1));word(e.dt_cursor,e.tree+56)
        tree=ctypes.string_at(e.phys+e.tree-e.virt,e.tree_size);mo,mn,so,sn,base,freq,_,_=tree_contract(tree)
        soc=bytearray(48);struct.pack_into('<Q',soc,0,e.soc_data+8);soc[8:8+sn]=tree[so:so+sn];struct.pack_into('<Q',soc,40,base);write(e.soc_data,soc)
        info=bytearray(0x208);info[0x110:0x110+mn]=tree[mo:mo+mn]
        for off,value in ((0x10,freq),(0x20,1),(0x30,2),(0x38,1),(0x48,freq)):
            struct.pack_into('<Q',info,0x140+off,value)
        write(e.pe_info,info);write(e.config_bits,struct.pack('<I',3))

    def valid(self,e):
        for name in ('xnu_cinit_valid','xnu_cpu_valid','xnu_platform_valid','xnu_pe_valid'):
            api=getattr(self.lib,name);api.argtypes=[ctypes.POINTER(Expected)]
            if not api(ctypes.byref(e)):return 0
        return 1

    def test_complete_globals_parent_frame_guard_and_tree_corruption(self):
        (payload,_),_=synthetic_payload();good,_,e=self.load(payload);self.assertEqual(good,1)
        self.assertEqual(e.mode,7);self.assertEqual(e.sp0,BASE+0x1b000-0xd0)
        self.assertEqual(self.valid(e),0);self.populate(e);self.assertEqual(self.valid(e),1)
        regions=((e.soc_data,48),(e.pe_info,0x208),(e.config_bits,4),(e.debug_enabled,4),(e.degradation,4),
                 (e.pe_state,0xb0),(e.dt_root,28),(e.dt_cursor,8),(e.sp0-16,0xe0),(e.sp0-0xf30,64),
                 (e.tree,e.tree_size))
        for va,size in regions:
            for i in range(size):
                byte=ctypes.c_uint8.from_address(e.phys+va+i-e.virt);byte.value^=1
                with self.subTest(region=va,byte=i):self.assertEqual(self.valid(e),0)
                byte.value^=1
        # Retired helper frames need not be zero, and must not be confused with active saves.
        ctypes.c_uint64.from_address(e.phys+e.sp0-0x80-e.virt).value=0x1234
        self.assertEqual(self.valid(e),1)

    def test_loader_rejects_metadata_tree_commandline_and_initial_values_before_writes(self):
        (payload,_),_=synthetic_payload();h=HEADER.unpack_from(payload);at=HEADER.size+h[2]*40
        bads=[]
        for offset,value in ((at+184,BASE+0x5600),(at+192,BASE+0x2000),(at+200,BASE+0x5900),
                             (at+208,BASE+0x5b0d),(at+216,BASE+0x5b10),(32,BASE+0x2434)):
            b=bytearray(payload);struct.pack_into('<Q',b,offset,value);bads.append(b)
        for offset in (128,h[7]+0x4a00,h[9]+tree_contract(adt())[2]):
            b=bytearray(payload);b[offset]^=0x80;bads.append(b)
        for bad in bads:
            ctypes.memset(self.address,0xaa,ARENA)
            self.assertEqual(self.load(bad)[0],0)
            self.assertEqual(ctypes.string_at(self.address,ARENA),b'\xaa'*ARENA)

    def test_capture_rejects_each_register_and_previous_modes(self):
        (payload,_),_=synthetic_payload();good,_,e=self.load(payload);self.assertEqual(good,1)
        c=cinittests.capture(e);c[18]=0xffffffff;c[19]=e.cpu_data;c[22]=e.c_entry+0x5b0
        api=self.lib.xnu_capture_valid;api.argtypes=[ctypes.POINTER(ctypes.c_uint64),ctypes.POINTER(Expected)]
        def valid(v):return api((ctypes.c_uint64*27)(*v),ctypes.byref(e))
        self.assertEqual(valid(c),1)
        for i in range(27):
            bad=c.copy();bad[i]^=1
            with self.subTest(register=i):self.assertEqual(valid(bad),0)
        for i,value in ((18,0),(18,2**64-1),(19,e.const_args),(3,0),(24,e.sp0+0x30),(2,c[2]|2)):
            bad=c.copy();bad[i]=value;self.assertEqual(valid(bad),0)

    def test_reader_requires_all_twenty_two_unique_success_fields(self):
        keys={'entry EL':2,'current EL':1,'stage':3,'handoff EL':2}
        keys.update({k:1 for k in ('XNU copied','XNU checkpoint','XNU boot_args verified','XNU current EL',
            'XNU MMU enabled','XNU tables verified','XNU translations verified','XNU C body verified',
            'XNU stack writes verified','XNU args copy verified','XNU cache maintenance verified','XNU CPU data verified',
            'XNU platform state verified','XNU ADT binding verified','XNU ADT lookup verified',
            'XNU SoC properties verified','XNU clock info verified','XNU PE return verified')})
        def record(items):
            data=('Trogdor-ipadOS native phase-0 probe\n'+''.join(f'{k}=0x{v:016x}\n' for k,v in items)).encode()
            return data+bytes(8192-len(data))
        self.assertEqual(len(keys),22);self.assertEqual(read_record(record(keys.items()),xnu_pe=True)[1],keys)
        for key in keys:
            for how in ('missing','zero','duplicate'):
                items=[(k,v) for k,v in keys.items() if k!=key]
                if how=='zero':items.append((key,0))
                if how=='duplicate':items.extend([(key,keys[key])]*2)
                with self.subTest(key=key,how=how),self.assertRaises(ValueError):read_record(record(items),xnu_pe=True)

    def test_exact_source_patch_loader_and_old_six_modes(self):
        if not cinittests.SOURCE.exists():self.skipTest('optional exact source absent')
        data=cinittests.SOURCE.read_bytes();payload,report=pack(data,adt(),pe_return=True);h=HEADER.unpack_from(payload)
        self.assertEqual(h[1],7);self.assertTrue(report['pe_return_checkpoint']);self.assertEqual(h[5],0xfffffff0079b3cb4)
        raw=h[7];diff=[i for i,(a,b) in enumerate(zip(data,payload[raw:raw+len(data)])) if a!=b]
        self.assertTrue(diff);self.assertTrue(all(report['patch_file_offset']<=i<report['patch_file_offset']+4 for i in diff))
        good,_,e=self.load(payload);self.assertEqual(good,1);self.populate(e);self.assertEqual(self.valid(e),1)
        for kwargs in ({},{'mmu':True},{'highva':True},{'cinit':True},{'cpu_init':True},{'platform':True}):
            self.assertEqual(self.load(pack(data,adt(),**kwargs)[0])[0],1)
        with self.assertRaisesRegex(ValueError,'command line'):pack(data,adt(),command_line='assertions=0',pe_return=True)
        with self.assertRaisesRegex(ValueError,'unaudited'):pack(data,adt(root={'target-type':b'test\0'}),pe_return=True)
        bad=bytearray(data);bad[-1]^=1
        with self.assertRaisesRegex(ValueError,'exact audited'):audit_metadata(bad,macho(data),adt(),COMMAND_LINE)

    def test_crosscompile_and_scalar_wrapper_linux_preservation(self):
        (payload,_),_=synthetic_payload()
        linux=struct.pack('<II6QII',0x91005a4d,0x14000001,0,4096,0xa,0,0,0,0x644d5241,0)+bytes(64)
        with tempfile.TemporaryDirectory() as tmp:
            image,r=compile_wrapper(Path(tmp),linux,4096,payload)
            self.assertEqual(r['xnu_checkpoint_version'],7);self.assertEqual(image[r['linux_offset']:],linux)
            self.assertLessEqual(r['header']['memory_size'],80*1024*1024)
