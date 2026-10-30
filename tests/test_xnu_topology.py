import ctypes
import hashlib
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch
import test_xnu_checkpoint as entrytests
import test_xnu_cinit as cinittests
import test_xnu_pe as petests
from test_xnu_checkpoint import ROOT,BASE,ARENA,Expected
from xnu_checkpoint import pack,HEADER,HVC
from xnu_topology import METADATA,SIZES,cpu_reg_offset,audit_metadata
from linux_log import compile_wrapper
from read_probe import read_record,read_diagnostic_record


def synthetic_payload():
    (prior,_),data=petests.synthetic_payload();data=bytearray(data)
    at=HEADER.size+HEADER.unpack_from(prior)[2]*40;tree=petests.adt()
    values=[BASE+i for i in (0x4a10,0x4a68,0x4c48,0x4c90,0x4c98,0x4c9c,0x4ca0,0x4ca4,0x4ca8)]
    struct.pack_into('<I',data,0x4a10,1);struct.pack_into('<2Q',data,0x4a28,values[1],values[2])
    data[0x4c98]=1;data[0x4c9c:0x4ca0]=b'\xff'*4
    meta=METADATA.pack(*values,cpu_reg_offset(tree))
    with patch('xnu_checkpoint.MMU_PREFIX_SHA256',hashlib.sha256(data[0x1100:0x1400]).hexdigest()), \
         patch('xnu_checkpoint.HIGHVA_PREFIX_SHA256',hashlib.sha256(data[0x1100:0x1430]).hexdigest()), \
         patch('xnu_cinit.audit_metadata',return_value=prior[at+48:at+96]), \
         patch('xnu_cpu.audit_metadata',return_value=prior[at+96:at+112]), \
         patch('xnu_platform.audit_metadata',return_value=(prior[at+112:at+184],BASE+0x2434)), \
         patch('xnu_pe.audit_metadata',return_value=(prior[at+184:at+224],BASE+0x21b0)), \
         patch('xnu_topology.audit_metadata',return_value=(meta,BASE+0x2248)):
        return pack(data,tree,topology=True),data


def record(items=None):
    values={'entry EL':2,'current EL':1,'stage':3,'handoff EL':2}
    values.update({key:1 for key in ('XNU copied','XNU checkpoint','XNU boot_args verified','XNU current EL',
        'XNU MMU enabled','XNU tables verified','XNU translations verified','XNU C body verified',
        'XNU stack writes verified','XNU args copy verified','XNU cache maintenance verified','XNU CPU data verified',
        'XNU platform state verified','XNU ADT binding verified','XNU ADT lookup verified','XNU SoC properties verified',
        'XNU clock info verified','XNU PE return verified','XNU WFE state verified','XNU CPU topology verified',
        'XNU system registers restored','XNU topology schema')})
    values.update({'XNU payload version':11,'XNU ELR':0xfffffff0079b3d50,'XNU WFE EVENTI':4})
    for name,before in (('CNTKCTL',0x100),('TPIDR',0x1234),('TPIDRRO',0x5678)):
        values['XNU '+name+' before']=before;values['XNU '+name+' after']=before
        values['XNU '+name+' live']=before|0x4f if name=='CNTKCTL' else 0
    values['XNU return boundary verified']=1;values['XNU diagnostic return']=0
    raw=('Trogdor-ipadOS native phase-0 probe\n'+''.join(f'{k}=0x{v:016x}\n' for k,v in (values.items() if items is None else items))).encode()
    return raw+bytes(8192-len(raw)),values


class XnuTopologyTests(unittest.TestCase):
    setUpClass=classmethod(entrytests.XnuCheckpointTests.setUpClass.__func__)
    tearDownClass=classmethod(entrytests.XnuCheckpointTests.tearDownClass.__func__)
    load=entrytests.XnuCheckpointTests.load

    def populate(self,e):
        petests.XnuPeTests.populate(self,e)
        def write(va,b):ctypes.memmove(e.phys+va-e.virt,bytes(b),len(b))
        write(e.sp0-8,struct.pack('<Q',e.c_entry+0x648))
        info=bytearray(88)
        struct.pack_into('<6I',info,0,1,1,0,1,0,0)
        struct.pack_into('<4Q',info,24,e.topo_cpus,e.topo_clusters,e.topo_cpus,e.topo_clusters)
        struct.pack_into('<8I',info,56,0,1,1,0,0,1,0,0)
        write(e.topo_info,info)
        cpus=bytearray(480);struct.pack_into('<I',cpus,4,e.topo_affinity)
        struct.pack_into('<2I',cpus,112,(e.topo_affinity>>8)&255,e.topo_affinity&255);write(e.topo_cpus,cpus)
        cluster=bytearray(72);struct.pack_into('<I',cluster,8,1);struct.pack_into('<Q',cluster,16,1);write(e.topo_clusters,cluster)
        for va,value,size in ((e.topo_offsets,0,8),(e.topo_correct,1,1),(e.topo_eventi,4,4),
                             (e.topo_page,12,4),(e.topo_userpage,12,4),(e.topo_stall,0,8)):
            write(va,value.to_bytes(size,'little'))

    def valid(self,e):
        if not petests.XnuPeTests.valid(self,e):return 0
        api=self.lib.xnu_topology_valid;api.argtypes=[ctypes.POINTER(Expected)]
        return api(ctypes.byref(e))

    def test_new_tag_stop_initializers_and_all_final_object_bytes(self):
        (payload,_),_=synthetic_payload();good,_,e=self.load(payload);self.assertEqual(good,1)
        self.assertEqual((e.mode,e.topology,e.diagnostics),(7,1,0));self.assertEqual(e.pc-e.phys+e.virt,e.c_entry+0x648)
        self.assertEqual(self.valid(e),0);self.populate(e);self.assertEqual(self.valid(e),1)
        regions=(e.topo_info,e.topo_cpus,e.topo_clusters,e.topo_offsets,e.topo_correct,e.topo_eventi,e.topo_page,e.topo_userpage,e.topo_stall)
        for va,size in zip(regions,SIZES):
            for i in range(size):
                byte=ctypes.c_uint8.from_address(e.phys+va-e.virt+i);byte.value^=1
                self.assertEqual(self.valid(e),0,(va,i));byte.value^=1
        for at in range(0xe0):
            byte=ctypes.c_uint8.from_address(e.phys+e.sp0-16-e.virt+at);byte.value^=1
            self.assertEqual(self.valid(e),0,at);byte.value^=1

    def test_every_new_input_region_metadata_and_cpu_reg_reject_before_writes(self):
        (payload,_),_=synthetic_payload();h=HEADER.unpack_from(payload);ext=HEADER.size+h[2]*40
        meta=list(METADATA.unpack_from(payload,ext+224));bads=[]
        for i in range(9):
            for value in (0,BASE+0x2000,meta[(i+1)%9],2**64-8,meta[i]+1):
                b=bytearray(payload);struct.pack_into('<Q',b,ext+224+i*8,value);bads.append(b)
            for byte in {0,SIZES[i]-1}:
                b=bytearray(payload);b[h[7]+meta[i]-BASE+byte]^=1;bads.append(b)
        for value in (0,meta[9]+4,len(payload),2**64-1):
            b=bytearray(payload);struct.pack_into('<Q',b,ext+296,value);bads.append(b)
        b=bytearray(payload);b[h[9]+meta[9]]=1;bads.append(b)
        b=bytearray(payload);struct.pack_into('<Q',b,32,BASE+0x21b0);bads.append(b)
        b=bytearray(payload);struct.pack_into('<I',b,8,7);bads.append(b)
        for prot in (0,2,5):
            b=bytearray(payload);struct.pack_into('<Q',b,HEADER.size+40+32,prot);bads.append(b) # unreadable or executable data descriptor
        for b in bads:
            ctypes.memset(self.address,0xaa,ARENA)
            self.assertEqual(self.load(b)[0],0)
            self.assertEqual(ctypes.string_at(self.address,ARENA),b'\xaa'*ARENA)

    def test_independent_cpu_reg_scan_and_affinity(self):
        tree=petests.adt();offset=cpu_reg_offset(tree)
        api=self.lib.xnu_topology_reg_offset;api.argtypes=[ctypes.c_void_p,ctypes.c_size_t];api.restype=ctypes.c_uint64
        self.assertEqual(api(ctypes.create_string_buffer(tree),len(tree)),offset)
        for n in range(len(tree)):self.assertEqual(api(ctypes.create_string_buffer(tree[:n]),n),0)
        for i in (offset-36,offset-4):
            bad=bytearray(tree);bad[i]^=0x80;self.assertEqual(api(ctypes.create_string_buffer(bytes(bad)),len(bad)),0)

    def test_capture_tag_keeps_all_twenty_seven_gates(self):
        (payload,_),_=synthetic_payload();_,_,e=self.load(payload)
        c=cinittests.capture(e);c[18]=0;c[19]=e.cpu_data;c[22]=e.c_entry+0x648
        api=self.lib.xnu_capture_valid;api.argtypes=[ctypes.POINTER(ctypes.c_uint64),ctypes.POINTER(Expected)]
        def valid(v):return api((ctypes.c_uint64*27)(*v),ctypes.byref(e))
        self.assertEqual(valid(c),1)
        for i in range(27):
            bad=c.copy();bad[i]^=1;self.assertEqual(valid(bad),0,i)
        e.topology=0;self.assertEqual(valid(c),0)

    def test_nonzero_wfe_register_initial_values_and_corruption(self):
        api=self.lib.xnu_wfe_valid;api.argtypes=[ctypes.POINTER(ctypes.c_uint64),ctypes.POINTER(ctypes.c_uint64)]
        for before in (0,0x100,0x321,0x3ff):
            saved=(ctypes.c_uint64*18)();saved[17]=before
            live=[before|0x4f,0,0]
            self.assertEqual(api((ctypes.c_uint64*3)(*live),saved),1)
            for i in range(3):
                for bit in range(64):
                    bad=live.copy();bad[i]^=1<<bit;self.assertEqual(api((ctypes.c_uint64*3)(*bad),saved),0)

    def test_reader_requires_all_fields_and_register_relations_and_legacy_rejection(self):
        data,values=record();self.assertEqual(len(read_record(data,xnu_topology=True)[1]),40)
        for key in values:
            for how in ('missing','duplicate','corrupt'):
                items=[(k,v) for k,v in values.items() if k!=key]
                if how=='duplicate':items.extend([(key,values[key])]*2)
                if how=='corrupt':items.append((key,values[key]^1))
                with self.subTest(key=key,how=how),self.assertRaises(ValueError):read_record(record(items)[0],xnu_topology=True)
        for kwargs in ({'xnu_pe':True},{'xnu_soc':True},{'xnu_platform':True}):
            with self.assertRaises(ValueError):read_record(data,**kwargs)
        with self.assertRaises(ValueError):read_diagnostic_record(data)
        # Explicit version11 must also reject through legacy mode when all new marker lines are stripped.
        stripped=[(k,v) for k,v in values.items() if k not in ('XNU topology schema','XNU CPU topology verified','XNU WFE state verified','XNU system registers restored')]
        with self.assertRaises(ValueError):read_record(record(stripped)[0],xnu_pe=True)

    def test_exact_source_single_patch_original_vectors_and_all_legacy_payloads(self):
        if not cinittests.SOURCE.exists():self.skipTest('optional exact source absent')
        source=cinittests.SOURCE.read_bytes();payload,report=pack(source,petests.adt(),topology=True);h=HEADER.unpack_from(payload)
        self.assertEqual((h[1],h[5]),(11,0xfffffff0079b3d4c))
        modified=payload[h[7]:h[7]+len(source)]
        diff=[i for i,(a,b) in enumerate(zip(source,modified)) if a!=b]
        self.assertTrue(diff);self.assertTrue(all(report['patch_file_offset']<=i<report['patch_file_offset']+4 for i in diff))
        self.assertEqual(struct.unpack_from('<I',modified,report['patch_file_offset'])[0],HVC)
        self.assertEqual(self.load(payload)[0],1)
        for kwargs in ({},{'mmu':True},{'highva':True},{'cinit':True},{'cpu_init':True},{'platform':True},
                       {'pe_return':True},{'soc_properties':True},{'diagnostics':True},{'diagnostic_control':True}):
            self.assertEqual(self.load(pack(source,petests.adt(),**kwargs)[0])[0],1,kwargs)
        with self.assertRaises(ValueError):pack(source,petests.adt(),topology=True,diagnostics=True)
        bad=bytearray(source);bad[100]^=1
        with self.assertRaises(ValueError):audit_metadata(bytes(bad),macho_bad(),petests.adt())

    def test_scalar_wrapper_and_restore_generated_code(self):
        (payload,_),_=synthetic_payload()
        linux=struct.pack('<II6QII',0x91005a4d,0x14000001,0,4096,0xa,0,0,0,0x644d5241,0)+bytes(64)
        with tempfile.TemporaryDirectory() as tmp:
            image,r=compile_wrapper(Path(tmp),linux,4096,payload)
            self.assertEqual(r['xnu_checkpoint_version'],11);self.assertEqual(image[r['linux_offset']:],linux)
            self.assertEqual(len(r['c_relocations']),13)


# A changed source must fail the pinned audit before any fake metadata is used.
def macho_bad():
    from xnu_macho import macho
    return macho(cinittests.SOURCE.read_bytes())
