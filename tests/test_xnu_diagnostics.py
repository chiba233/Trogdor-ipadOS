import ctypes
from pathlib import Path
import struct
import tempfile
import unittest
import test_xnu_checkpoint as entrytests
import test_xnu_cinit as cinittests
import test_xnu_soc as soctests
from test_xnu_checkpoint import BASE,ARENA,Expected
from test_xnu_mmu import expected_at,tables
from test_xnu_pe import adt
from xnu_checkpoint import pack,HEADER,HVC
from xnu_diagnostics import FAULT_HVC
from xnu_macho import macho,backed
from linux_log import compile_wrapper
from read_probe import read_record,read_diagnostic_record


class XnuDiagnosticsTests(unittest.TestCase):
    setUpClass=classmethod(entrytests.XnuCheckpointTests.setUpClass.__func__)
    tearDownClass=classmethod(entrytests.XnuCheckpointTests.tearDownClass.__func__)
    load=entrytests.XnuCheckpointTests.load

    def scene(self,fault=False,version=9):
        e=expected_at(0x84000000);e.mode=8;e.diagnostics=version;e.end=e.phys+0x1400000
        e.kernel_end=e.virt+0x1310000;e.soc=0xfffffff0079483dc;e.vbar=0xfffffff0073ed000
        e.pc=e.phys+e.soc+0xb8-e.virt;e.soc_data=0xfffffff007b320c8;e.soc_base=0x17000000
        c=soctests.capture(e);f=[0]*5
        if fault:
            c[0]=e.vbar+4;c[1]=0x3c5;c[17]=(e.phys+c[0]-e.virt)&~4095;c[23]=0
            f=[0xf2005846,e.soc+0x34,0,0x3c4,1]
        return e,c,f,tables(e)

    def transport(self,e,c,f,pool):
        api=self.lib.xnu_diagnostic_return_valid
        api.argtypes=[ctypes.POINTER(ctypes.c_uint64),ctypes.POINTER(ctypes.c_uint64),ctypes.POINTER(Expected),ctypes.POINTER(ctypes.c_uint64)]
        return api((ctypes.c_uint64*27)(*c),pool,ctypes.byref(e),(ctypes.c_uint64*5)(*f))

    def test_normal_failed_capture_and_fault_have_separate_transport(self):
        for fault in (False,True):
            e,c,f,pool=self.scene(fault);self.assertEqual(self.transport(e,c,f,pool),1)
            for slot in (3,4,5,6,7,8,13,14,18,19,20,21,22,23,24):
                bad=c.copy();bad[slot]^=0xdeadbeef
                self.assertEqual(self.transport(e,bad,f,pool),1,slot)
            for slot in (0,1,2,9,10,11,12,15,16,17,25,26):
                bad=c.copy();bad[slot]^=1 if slot!=2 else 4
                self.assertEqual(self.transport(e,bad,f,pool),0,slot)
        e,c,f,pool=self.scene(True,10);self.assertEqual(self.transport(e,c,f,pool),1)
        e,c,f,pool=self.scene(False,10);self.assertEqual(self.transport(e,c,f,pool),0)

    def test_full_table_pool_and_context_bounds_remain_hard_gates(self):
        e,c,f,pool=self.scene(True)
        for i in [j for j in range(8192) if pool[j]]+[8191]:
            pool[i]^=1;self.assertEqual(self.transport(e,c,f,pool),0,i);pool[i]^=1
        for key,value in (('mode',7),('diagnostics',0),('diagnostics',11),('pages',e.end-8),
                          ('args',e.end-8),('pc',e.end),('end',e.phys-1),('vbar',e.vbar+4),
                          ('kernel_end',e.virt),('kernel_end',e.end-e.phys+e.virt+1)):
            old=getattr(e,key);setattr(e,key,value);self.assertEqual(self.transport(e,c,f,pool),0,key);setattr(e,key,old)
        for i,value in ((1,e.soc+1),(1,e.soc_data),(1,e.kernel_end),(3,0x3d4),(3,0x3c0),(4,2)):
            bad=f.copy();bad[i]=value;self.assertEqual(self.transport(e,c,bad,pool),0,(i,value))
        # SP1 origin needs its own exact slot; M[4] is not a valid AArch64 mode.
        bad=f.copy();bad[3]=0x3c5;self.assertEqual(self.transport(e,c,bad,pool),0)
        c[0]=e.vbar+0x204;c[17]=(e.phys+c[0]-e.virt)&~4095
        self.assertEqual(self.transport(e,c,bad,pool),1)
        e,c,f,pool=self.scene(False)
        f[0]=1;self.assertEqual(self.transport(e,c,f,pool),0)

    def test_deliberate_control_pins_syndrome_and_fault_position(self):
        e,c,f,pool=self.scene(True,10)
        for i in (0,1):
            bad=f.copy();bad[i]^=4;self.assertEqual(self.transport(e,c,bad,pool),0)
        api=self.lib.xnu_capture_valid;api.argtypes=[ctypes.POINTER(ctypes.c_uint64),ctypes.POINTER(Expected)]
        self.assertEqual(api((ctypes.c_uint64*27)(*c),ctypes.byref(e)),0)

    def payload(self,control=False):
        if not cinittests.SOURCE.exists():self.skipTest('optional audited source absent')
        data=cinittests.SOURCE.read_bytes();return data,pack(data,adt(),diagnostics=not control,diagnostic_control=control)

    def test_exact_source_three_or_four_patches_and_semantic_mode8(self):
        for control in (False,True):
            data,(payload,r)=self.payload(control);h=HEADER.unpack_from(payload)
            self.assertEqual(h[1],10 if control else 9);self.assertEqual(len(r['fault_vector_patches']),3 if control else 2)
            patches=[(r['patch_file_offset'],HVC),*[(p['file_offset'],0xd42b08c0 if p.get('deliberate_fault') else FAULT_HVC) for p in r['fault_vector_patches']]]
            allowed={at+i for at,_ in patches for i in range(4)}
            for at,word in patches:self.assertEqual(struct.unpack_from('<I',payload,h[7]+at)[0],word)
            diff={i for i,(a,b) in enumerate(zip(data,payload[h[7]:h[7]+len(data)])) if a!=b}
            self.assertTrue(diff);self.assertTrue(diff<=allowed)
            good,_,e=self.load(payload);self.assertEqual(good,1);self.assertEqual(e.mode,8);self.assertEqual(e.diagnostics,h[1])
            if not control:
                soctests.XnuSocTests.populate(self,e);self.assertEqual(soctests.XnuSocTests.valid(self,e),1)
        with self.assertRaises(ValueError):pack(data,adt(),diagnostics=True,pe_return=True)

    def test_loader_rejects_vectors_control_and_executable_backing_before_writes(self):
        _,(payload,r)=self.payload(True);h=HEADER.unpack_from(payload)
        offsets=[h[7]+p['file_offset'] for p in r['fault_vector_patches']]
        bads=[]
        for offset in offsets:
            b=bytearray(payload);b[offset]^=1;bads.append(b)
        b=bytearray(payload);struct.pack_into('<I',b,8,9);bads.append(b)
        # File backing and executable permission are mandatory for every audited region.
        for i in range(h[2]):
            at=HEADER.size+i*40;off,extent,_,length,prot=struct.unpack_from('<5Q',payload,at)
            if h[3]+off<=0xfffffff007a14990<h[3]+off+extent:
                self.assertTrue(prot&4)
                b=bytearray(payload);struct.pack_into('<Q',b,at+32,prot&~4);bads.append(b)
                # All earlier call/checkpoint prefixes remain backed, but memcpy does not.
                b=bytearray(payload);struct.pack_into('<Q',b,at+24,0xfffffff007a14990-h[3]-off);bads.append(b)
                break
        else:self.fail('audited memcpy segment absent')
        for b in bads:
            ctypes.memset(self.address,0xaa,ARENA);self.assertEqual(self.load(b)[0],0)
            self.assertEqual(ctypes.string_at(self.address,ARENA),b'\xaa'*ARENA)
        normal=bytearray(payload);struct.pack_into('<I',normal,8,8)
        # Existing mode8 is independent of new diagnostic transport flags.
        self.assertEqual(self.load(normal)[2].diagnostics,0)

    def test_code_allowlist_is_inside_actual_executable_file_ranges(self):
        data,_=self.payload();kernel=macho(data)
        api=self.lib.xnu_diagnostic_code_mask;api.argtypes=[ctypes.c_uint64,ctypes.c_uint64];api.restype=ctypes.c_uint64
        mask=0
        for va,_,_,length,prot in kernel.segments:
            if prot&4:mask|=api(va,length)
        self.assertEqual(mask,(1<<18)-1)
        self.assertEqual(api(2**64-8,16),0)

    def record(self,reason=1):
        e,c,f,_=self.scene(reason==1,10 if reason==1 else 9)
        fields={'entry EL':2,'current EL':1,'stage':3,'handoff EL':2,'XNU copied':1,
            'XNU diagnostic schema':1,'XNU payload version':e.diagnostics,'XNU diagnostic transport verified':1,
            'XNU table pool verified':1,'XNU capture verified':int(reason not in (1,2)),
            'XNU fault received':int(reason==1),'XNU checkpoint':int(reason==0),
            'XNU diagnostic return':int(reason!=0),'XNU return boundary verified':int(reason not in (1,2)),
            'XNU diagnostic reason':reason}
        fields.update({'XNU fault '+name:value for name,value in zip(('ESR','ELR','FAR','SPSR'),f[:4])})
        fields.update({f'XNU capture {i:02}':value for i,value in enumerate(c)})
        return fields

    def encode(self,items):
        b=('Trogdor-ipadOS native phase-0 probe\n'+''.join(f'{k}=0x{v:016x}\n' for k,v in items)).encode()
        return b+bytes(8192-len(b))

    def test_diagnostic_reader_requires_all_raw_fields_and_consistent_failure(self):
        for reason in (1,2,3):
            fields=self.record(reason);raw=self.encode(fields.items())
            self.assertEqual(read_diagnostic_record(raw)[1]['XNU diagnostic reason'],reason)
            with self.assertRaises(ValueError):read_record(raw,xnu_soc=True)
        fields=self.record(1)
        for key in fields:
            for how in ('missing','duplicate'):
                items=[(k,v) for k,v in fields.items() if k!=key]
                if how=='duplicate':items.extend([(key,fields[key])]*2)
                with self.subTest(key=key,how=how),self.assertRaises(ValueError):read_diagnostic_record(self.encode(items))
        for key,value in (('XNU diagnostic transport verified',0),('XNU fault received',0),
            ('XNU checkpoint',1),('XNU diagnostic reason',3),('XNU diagnostic return',0),
            ('XNU fault SPSR',0x3d4),('XNU payload version',8),('XNU fault ESR',0),
            ('XNU fault ELR',0),('XNU capture 00',0),('XNU capture 01',0),('XNU capture verified',1)):
            bad=dict(fields);bad[key]=value
            with self.subTest(key=key),self.assertRaises(ValueError):read_diagnostic_record(self.encode(bad.items()))

    def test_success_reader_does_not_accept_diagnostic_status_without_twenty_fields(self):
        fields=self.record(0)
        with self.assertRaises(ValueError):read_diagnostic_record(self.encode(fields.items()))
        fields.update({k:1 for k in ('XNU boot_args verified','XNU current EL','XNU MMU enabled',
            'XNU tables verified','XNU translations verified','XNU C body verified','XNU stack writes verified',
            'XNU args copy verified','XNU cache maintenance verified','XNU CPU data verified',
            'XNU platform state verified','XNU ADT binding verified','XNU ADT lookup verified','XNU SoC properties verified')})
        raw=self.encode(fields.items());self.assertEqual(read_diagnostic_record(raw)[1]['XNU checkpoint'],1)
        self.assertEqual(len(read_record(raw,xnu_soc=True)[1]),20)

    def test_actual_control_wrapper_scalar_bounds_and_linux_preservation(self):
        _,(payload,_)=self.payload(True)
        linux=struct.pack('<II6QII',0x91005a4d,0x14000001,0,4096,0xa,0,0,0,0x644d5241,0)+bytes(64)
        with tempfile.TemporaryDirectory() as tmp:
            image,r=compile_wrapper(Path(tmp),linux,4096,payload)
            self.assertEqual(r['xnu_checkpoint_version'],10);self.assertEqual(image[r['linux_offset']:],linux)
            self.assertLessEqual(r['header']['memory_size'],80*1024*1024)
