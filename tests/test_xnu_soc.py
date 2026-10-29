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
import test_xnu_pe as petests
from test_xnu_checkpoint import BASE,ARENA,Expected
from xnu_checkpoint import pack,HEADER,HVC
from xnu_soc import ranges_offset
from xnu_pe import tree_contract
from xnu_platform import search_offsets
from linux_log import compile_wrapper
from read_probe import read_record


def synthetic_payload():
    tree=petests.adt();(prior,_),data=petests.synthetic_payload(tree)
    at=HEADER.size+HEADER.unpack_from(prior)[2]*40
    metadata=prior[at+184:at+224]+struct.pack('<Q',ranges_offset(tree))
    with patch('xnu_checkpoint.MMU_PREFIX_SHA256',hashlib.sha256(data[0x1100:0x1400]).hexdigest()), \
         patch('xnu_checkpoint.HIGHVA_PREFIX_SHA256',hashlib.sha256(data[0x1100:0x1430]).hexdigest()), \
         patch('xnu_cinit.audit_metadata',return_value=prior[at+48:at+96]), \
         patch('xnu_cpu.audit_metadata',return_value=prior[at+96:at+112]), \
         patch('xnu_platform.audit_metadata',return_value=(prior[at+112:at+184],BASE+0x2434)), \
         patch('xnu_soc.audit_metadata',return_value=(metadata,BASE+0x24b8)):
        return pack(data,tree,soc_properties=True),data


def capture(e):
    c=cinittests.capture(e)
    c[3]=e.soc_data&~4095;c[18]=e.soc_base;c[19]=e.soc_data+8
    c[22]=e.soc+0xa8;c[24]=e.sp0+0x30
    return c


class XnuSocTests(unittest.TestCase):
    setUpClass=classmethod(entrytests.XnuCheckpointTests.setUpClass.__func__)
    tearDownClass=classmethod(entrytests.XnuCheckpointTests.tearDownClass.__func__)
    load=entrytests.XnuCheckpointTests.load
    valid=petests.XnuPeTests.valid

    def populate(self,e):
        platformtests.XnuPlatformTests.populate(self,e)
        tree=ctypes.string_at(e.phys+e.tree-e.virt,e.tree_size)
        _,_,offset,size,base,_,_,_=tree_contract(tree)
        soc=bytearray(48);struct.pack_into('<Q',soc,0,e.soc_data+8)
        soc[8:8+size]=tree[offset:offset+size];struct.pack_into('<Q',soc,40,base)
        ctypes.memmove(e.phys+e.soc_data-e.virt,bytes(soc),48)
        slots=struct.pack('<3Q',e.tree+offset,24<<32,e.tree+e.soc_ranges)
        ctypes.memmove(e.phys+e.sp0-e.virt,slots,len(slots))

    def test_independent_ranges_scan_rejects_bad_names_lengths_and_bounds(self):
        tree=petests.adt();node,_=search_offsets(tree)
        api=self.lib.xnu_soc_ranges_offset;api.argtypes=[ctypes.c_void_p,ctypes.c_size_t,ctypes.c_uint64]
        api.restype=ctypes.c_uint64
        def scan(data,n=node):return api(ctypes.create_string_buffer(data),len(data),n)
        offset=ranges_offset(tree);self.assertEqual(scan(tree),offset)
        for n in (0,len(tree),2**64-1):self.assertEqual(scan(tree,n),0)
        for size in (0,16,25,2**32-1):
            bad=bytearray(tree);struct.pack_into('<I',bad,offset-4,size)
            self.assertEqual(scan(bytes(bad)),0)
            with self.assertRaises(ValueError):ranges_offset(bad)
        bad=bytearray(tree);bad[offset-36]=ord('x');self.assertEqual(scan(bytes(bad)),0)
        for n in range(offset+24):self.assertEqual(scan(tree[:n]),0)

    def test_patch_and_zero_cpu_and_metadata_reject_before_writes(self):
        (payload,report),data=synthetic_payload();h=HEADER.unpack_from(payload);at=HEADER.size+h[2]*40
        self.assertEqual(h[1],8);self.assertTrue(report['soc_properties_checkpoint'])
        self.assertEqual(struct.unpack_from('<I',payload,h[7]+0x24b8)[0],HVC)
        good,_,e=self.load(payload);self.assertEqual(good,1)
        self.assertEqual(e.sp0,BASE+0x1b000-0x200)
        self.assertEqual(ctypes.string_at(e.phys+e.cpu_data-e.virt,0x500),bytes(0x500))
        bads=[]
        for value in (0,2**64-1,ranges_offset(petests.adt())+4,tree_contract(petests.adt())[2]):
            bad=bytearray(payload);struct.pack_into('<Q',bad,at+224,value);bads.append(bad)
        for offset in (96,104):
            bad=bytearray(payload);struct.pack_into('<Q',bad,offset,ranges_offset(petests.adt()));bads.append(bad)
        for bad in bads:
            ctypes.memset(self.address,0xaa,ARENA);self.assertEqual(self.load(bad)[0],0)
            self.assertEqual(ctypes.string_at(self.address,ARENA),b'\xaa'*ARENA)

    def test_active_frames_soc_cache_and_untouched_later_globals(self):
        (payload,_),_=synthetic_payload();self.assertEqual(self.load(payload)[0],1);_,_,e=self.load(payload)
        self.populate(e);self.assertEqual(self.valid(e),1)
        for va,size in ((e.soc_data,48),(e.pe_info,0x208),(e.config_bits,4),(e.debug_enabled,4),
                        (e.degradation,4),(e.sp0,0x200),(e.sp0-0xe00,64),(e.tree,e.tree_size)):
            for i in range(size):
                byte=ctypes.c_uint8.from_address(e.phys+va+i-e.virt);byte.value^=1
                with self.subTest(region=va,byte=i):self.assertEqual(self.valid(e),0)
                byte.value^=1

    def test_capture_and_diagnostic_return_keep_every_strict_gate(self):
        from test_xnu_mmu import expected_at,tables
        e=expected_at(0x84000000);e.mode=8;e.soc_data=BASE+0x5800;e.soc=BASE+0x2400;e.soc_base=0x17000000
        c=capture(e);pool=tables(e)
        api=self.lib.xnu_pe_return_valid
        api.argtypes=[ctypes.POINTER(ctypes.c_uint64),ctypes.POINTER(ctypes.c_uint64),ctypes.POINTER(Expected)]
        def valid(v):return api((ctypes.c_uint64*27)(*v),pool,ctypes.byref(e))
        self.assertEqual(valid(c),1)
        for i in range(27):
            bad=c.copy();bad[i]^=1;self.assertEqual(valid(bad),0,i)
        for i in [j for j in range(8192) if pool[j]]+[8191]:
            pool[i]^=1;self.assertEqual(valid(c),0,i);pool[i]^=1
        for i,value in ((18,0xffffffff),(19,e.cpu_data),(22,e.soc+0xb8),(24,e.sp0+0xc0)):
            bad=c.copy();bad[i]=value;self.assertEqual(valid(bad),0)

    def test_reader_requires_twenty_fields_and_rejects_later_stage_claims(self):
        keys={'entry EL':2,'current EL':1,'stage':3,'handoff EL':2}
        keys.update({k:1 for k in ('XNU copied','XNU checkpoint','XNU boot_args verified','XNU current EL',
            'XNU MMU enabled','XNU tables verified','XNU translations verified','XNU C body verified',
            'XNU stack writes verified','XNU args copy verified','XNU cache maintenance verified','XNU CPU data verified',
            'XNU platform state verified','XNU ADT binding verified','XNU ADT lookup verified','XNU SoC properties verified')})
        def record(items):
            raw=('Trogdor-ipadOS native phase-0 probe\n'+''.join(f'{k}=0x{v:016x}\n' for k,v in items)).encode()
            return raw+bytes(8192-len(raw))
        self.assertEqual(len(keys),20);self.assertEqual(read_record(record(keys.items()),xnu_soc=True)[1],keys)
        for extra in ('XNU clock info verified','XNU PE return verified','XNU diagnostic return'):
            with self.assertRaises(ValueError):read_record(record([*keys.items(),(extra,1)]),xnu_soc=True)
        for key in keys:
            for how in ('missing','zero','duplicate'):
                items=[(k,v) for k,v in keys.items() if k!=key]
                if how=='zero':items.append((key,0))
                if how=='duplicate':items.extend([(key,keys[key])]*2)
                with self.subTest(key=key,how=how),self.assertRaises(ValueError):read_record(record(items),xnu_soc=True)

    def test_exact_source_single_patch_and_seven_older_modes(self):
        if not cinittests.SOURCE.exists():self.skipTest('optional exact source absent')
        data=cinittests.SOURCE.read_bytes();payload,r=pack(data,petests.adt(),soc_properties=True);h=HEADER.unpack_from(payload)
        self.assertEqual(h[5],0xfffffff007948494)
        self.assertEqual(struct.unpack_from('<I',data,r['patch_file_offset'])[0],0xa9437bfd)
        differences=[i for i,(a,b) in enumerate(zip(data,payload[h[7]:h[7]+len(data)])) if a!=b]
        self.assertTrue(differences);self.assertTrue(all(r['patch_file_offset']<=i<r['patch_file_offset']+4 for i in differences))
        good,_,e=self.load(payload);self.assertEqual(good,1);self.populate(e);self.assertEqual(self.valid(e),1)
        for kwargs in ({},{'mmu':True},{'highva':True},{'cinit':True},{'cpu_init':True},{'platform':True},{'pe_return':True}):
            self.assertEqual(self.load(pack(data,petests.adt(),**kwargs)[0])[0],1)
        with self.assertRaises(ValueError):pack(data,petests.adt(),pe_return=True,soc_properties=True)

    def test_scalar_wrapper_preserves_linux_and_memory_bound(self):
        (payload,_),_=synthetic_payload()
        linux=struct.pack('<II6QII',0x91005a4d,0x14000001,0,4096,0xa,0,0,0,0x644d5241,0)+bytes(64)
        with tempfile.TemporaryDirectory() as tmp:
            image,r=compile_wrapper(Path(tmp),linux,4096,payload)
            self.assertEqual(r['xnu_checkpoint_version'],8);self.assertEqual(image[r['linux_offset']:],linux)
            self.assertLessEqual(r['header']['memory_size'],80*1024*1024)
