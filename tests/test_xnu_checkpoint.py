import ctypes
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from fdt import Node, decode, encode, u32
from xnu_checkpoint import pack, HEADER, adt_fields
from xnu_macho import macho
from linux_log import compile_wrapper
from read_probe import read_record

BASE = 0xfffffff007004000
ARENA = 24 * 1024 * 1024


def adrp(reg, pc, target):
    delta = ((target & ~4095) - (pc & ~4095)) // 4096 & 0x1fffff
    return 0x90000000 | ((delta & 3) << 29) | ((delta >> 2) << 5) | reg


def fixture():
    commands = []
    specs = [(b'__TEXT', BASE, 0x4000, 0, 0x4000, 5, [(BASE+0x1000, 0x400)]),
             (b'__DATA', BASE+0x4000, 0x8000, 0x4000, 0x1000, 3,
              [(BASE+0x4000, 0x1000), (BASE+0x5000, 0x7000)]),
             (b'__LINKEDIT', BASE+0xc000, 0x1000, 0x5000, 0x1000, 1, [])]
    for name, va, mem, off, size, prot, sections in specs:
        command = bytearray(struct.pack('<II16s4Q4I', 0x19, 72+80*len(sections), name,
                                        va, mem, off, size, 7, prot, len(sections), 0))
        for addr, extent in sections:
            section = bytearray(80)
            struct.pack_into('<QQ', section, 32, addr, extent)
            command += section
        commands.append(command)
    thread = bytearray(288)
    struct.pack_into('<4I', thread, 0, 5, 288, 6, 68)
    struct.pack_into('<Q', thread, 272, BASE+0x1000)
    commands.append(thread)
    names = b'\0_start_first_cpu\0_bootstrap_pagetables\0'
    commands.append(struct.pack('<6I', 2, 24, 0x5100, 2, 0x5200, len(names)))
    dysym = bytearray(80)
    struct.pack_into('<II', dysym, 0, 0xb, 80)
    struct.pack_into('<II', dysym, 72, 0x5000, 1)
    commands.append(dysym)
    data = bytearray(0x6000)
    struct.pack_into('<8I', data, 0, 0xfeedfacf, 0x100000c, 0, 2, len(commands),
                     sum(map(len, commands)), 0, 0)
    data[32:32+sum(map(len, commands))] = b''.join(commands)
    struct.pack_into('<I', data, 0x1000, 0x14000040)
    words = [0xd510109f, 0xd5034fdf, 0xaa0003f4, 0xd2800015,
             adrp(0, BASE+0x1110, BASE+0x2000), 0x91000000, 0xd518c000,
             0xf9400696, 0xf9400a97, 0xf9400e98,
             adrp(25, BASE+0x1128, BASE+0x8000), 0xf9423a9a, 0xd51bd07f]
    struct.pack_into('<13I', data, 0x1100, *words)
    struct.pack_into('<Q', data, 0x4000, BASE+0x1100)
    struct.pack_into('<iI', data, 0x5000, 0x4000, 0x06000001)
    struct.pack_into('<IBBHQ', data, 0x5100, 1, 0xf, 1, 0, BASE+0x1100)
    struct.pack_into('<IBBHQ', data, 0x5110, 18, 0xf, 3, 0, BASE+0x8000)
    data[0x5200:0x5200+len(names)] = names
    return data


def adt_node(name, props=None, children=()):
    props = {'name': name.encode()+b'\0', **(props or {})}
    out = struct.pack('<II', len(props), len(children))
    for key, value in props.items():
        out += key.encode().ljust(32,b'\0') + struct.pack('<I',len(value)) + value
        out += bytes(-len(value) % 4)
    return out + b''.join(children)


def adt():
    return adt_node('', children=[adt_node('chosen', {'dram-base': bytes(8), 'dram-size': bytes(8)}),
        adt_node('cpus', children=[adt_node('cpu0', {'reg':bytes(4),
                 'timebase-frequency': struct.pack('<I',19200000)})])])


class Handoff(ctypes.Structure):
    _fields_ = [('entry', ctypes.c_uint64), ('args', ctypes.c_uint64)]


class Expected(ctypes.Structure):
    _fields_ = [(key, ctypes.c_uint64) for key in ('pc','args','virt','phys','memory','pages','flags','mode','tcr','mair','vbar','sp0','sp1','end','c_entry','const_args','boot_args','slide','static_cache','guard','cpu_data','rtclock','pe_state','protection','dt_root','dt_cursor','pe_init','identify','soc','node','cursor','tree','tree_size','soc_data','pe_info','config_bits','debug_enabled','degradation','adt_checksum','soc_ranges','soc_base','diagnostics','kernel_end','topology','topo_info','topo_cpus','topo_clusters','topo_offsets','topo_correct','topo_eventi','topo_page','topo_userpage','topo_stall','topo_reg','topo_affinity')]


class XnuCheckpointTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        lib = Path(cls.temp.name)/'loader.dylib'
        subprocess.run(['clang','-shared','-fPIC','-Wall','-Wextra','-Werror',
                        *[str(ROOT/'boot'/f) for f in ('xnu_load.c','xnu_memory.c','xnu_tables.c','xnu_cinit.c','xnu_cpu.c','xnu_platform.c','xnu_pe.c','xnu_diagnostics.c','xnu_topology.c','fdt.c')],
                        '-o',str(lib)],check=True)
        cls.lib = ctypes.CDLL(str(lib))
        cls.lib.xnu_load.argtypes = [ctypes.c_void_p,ctypes.c_size_t,ctypes.c_void_p,
            ctypes.c_size_t,ctypes.c_uint64,ctypes.c_uint64,ctypes.POINTER(Handoff),ctypes.POINTER(Expected)]
        cls.lib.xnu_range_allowed.argtypes = [ctypes.c_void_p,ctypes.c_uint64,ctypes.c_uint64]
        cls.storage = ctypes.create_string_buffer(ARENA+0x200000)
        cls.address = (ctypes.addressof(cls.storage)+0x1fffff)&~0x1fffff

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def test_unslid_local_pointer_audit_and_four_byte_checkpoint(self):
        data = fixture()
        self.assertEqual(macho(data).local_relocations,1)
        payload, report = pack(data, adt())
        raw = HEADER.unpack_from(payload)[7]
        modified = payload[raw:raw+len(data)]
        self.assertEqual([i for i,(a,b) in enumerate(zip(data,modified)) if a!=b],
                         [0x1130,0x1131,0x1132,0x1133])
        self.assertEqual(struct.unpack_from('<Q',modified,0x4000)[0],BASE+0x1100)
        self.assertEqual(report['patch_bytes'],4)
        for flags in (0x0e000001,0x07000001,0x04000001,0x06000000,0x06000004,0x16000001):
            bad = bytearray(data);struct.pack_into('<I',bad,0x5004,flags)
            with self.subTest(flags=flags), self.assertRaises(ValueError): macho(bad)
        for offset,value in ((0x5000,0x9000),(0x4000,BASE+0x5000)):
            bad = bytearray(data);struct.pack_into('<I' if offset==0x5000 else '<Q',bad,offset,value)
            with self.assertRaises(ValueError): macho(bad)
        bad=bytearray(data);struct.pack_into('<I',bad,0x1130,0xd503201f)
        with self.assertRaises(ValueError): pack(bad,adt())

    def load(self,payload,freq=19200000):
        p=ctypes.create_string_buffer(bytes(payload));out=Handoff();expected=Expected()
        good=self.lib.xnu_load(p,len(payload),self.address,ARENA,0,freq,ctypes.byref(out),ctypes.byref(expected))
        return good,out,expected

    def test_loader_copies_bss_builds_args_and_preserves_virtual_pointer(self):
        payload,_=pack(fixture(),adt())
        ctypes.memset(self.address,0xaa,0x20000)
        good,out,expected=self.load(payload)
        self.assertEqual(good,1)
        self.assertEqual(out.entry,self.address+0x1000)
        self.assertEqual(ctypes.c_uint64.from_address(self.address+0x4000).value,BASE+0x1100)
        self.assertEqual(ctypes.string_at(self.address+0x5000,0x7000),bytes(0x7000))
        args=ctypes.string_at(out.args,0x480)
        self.assertEqual(struct.unpack_from('<HH',args),(2,2))
        self.assertEqual(struct.unpack_from('<3Q',args,8),(BASE,self.address,ARENA))
        self.assertEqual(expected.pages,self.address+0x8000)
        tree_va=struct.unpack_from('<Q',args,0x60)[0]
        tree=ctypes.string_at(self.address+tree_va-BASE,len(adt()))
        base_at,size_at,_,_=adt_fields(tree)
        self.assertEqual(struct.unpack_from('<Q',tree,base_at)[0],self.address)
        self.assertEqual(struct.unpack_from('<Q',tree,size_at)[0],ARENA)

    def test_checkpoint_capture_checks_each_register_and_mmu_state(self):
        payload,_=pack(fixture(),adt())
        good,_,e=self.load(payload)
        self.assertEqual(good,1)
        self.lib.xnu_capture_valid.argtypes=[ctypes.POINTER(ctypes.c_uint64),ctypes.POINTER(Expected)]
        values=[e.pc+4,0x3c5,0x30d00800,e.args,e.virt,e.phys,e.memory,e.pages,e.flags]
        def valid(v):
            return self.lib.xnu_capture_valid((ctypes.c_uint64*9)(*v),ctypes.byref(e))
        self.assertEqual(valid(values),1)
        for i in (0,1,3,4,5,6,7,8):
            bad=values.copy();bad[i]^=1
            with self.subTest(register=i):self.assertEqual(valid(bad),0)
        for bit in (1,4,0x1000):
            bad=values.copy();bad[2]|=bit
            self.assertEqual(valid(bad),0)

    def test_loader_rejects_before_any_arena_write(self):
        payload,_=pack(fixture(),adt())
        bad_inputs=[payload[:-1]]
        for offset,fmt,value in ((0,'<Q',0),(12,'<I',65),(16+9*8,'<Q',ARENA+1),
                                 (16+10*8,'<Q',0xffffffffffffffff),(HEADER.size+40,'<Q',0),
                                 (128,'<I',0x80808080)):
            bad=bytearray(payload);struct.pack_into(fmt,bad,offset,value);bad_inputs.append(bad)
        for bad in bad_inputs:
            ctypes.memset(self.address,0xaa,0x20000)
            self.assertEqual(self.load(bad)[0],0)
            self.assertEqual(ctypes.string_at(self.address,0x20000),b'\xaa'*0x20000)
        self.assertEqual(self.load(payload,1)[0],0)

    def tree(self, cb=0x70000000, reserved=(), reservations=(), initrd=None, memory=None):
        children=[Node('memory',{'reg':struct.pack('>QQ',*(memory or (self.address,ARENA)))}),
            Node('reserved-memory',{'#address-cells':u32(2),'#size-cells':u32(2),'ranges':b''},list(reserved)),
            Node('firmware',{'ranges':b''},[Node('coreboot',{'reg':struct.pack('>QQ',cb,0x1000)})])]
        if initrd:
            children.append(Node('chosen',{'linux,initrd-start':struct.pack('>Q',initrd),
                                          'linux,initrd-end':struct.pack('>Q',initrd+4096)}))
        return encode(Node('',{'#address-cells':u32(2),'#size-cells':u32(2)},children),reservations)

    def allowed(self,data,lo=None):
        buf=ctypes.create_string_buffer(bytes(data))
        return self.lib.xnu_range_allowed(buf,self.address if lo is None else lo,ARENA)

    def test_runtime_memory_guard_excludes_every_live_owner(self):
        self.assertEqual(self.allowed(self.tree()),1)
        for data in (self.tree(cb=self.address),self.tree(initrd=self.address),
            self.tree(reservations=[(self.address,4096)]),
            self.tree(reserved=[Node('static',{'reg':struct.pack('>QQ',self.address,4096)})]),
            self.tree(reserved=[Node('dynamic',{'size':struct.pack('>Q',4096),
                        'alloc-ranges':struct.pack('>QQ',self.address,ARENA)})]),
            self.tree(reserved=[Node('unbounded',{'size':struct.pack('>Q',4096)})]),
            self.tree(memory=(self.address+4096,ARENA)),self.tree(memory=(0xfffffffffffff000,0x2000))):
            with self.subTest(data=data[-64:]): self.assertEqual(self.allowed(data),0)
        self.assertEqual(self.allowed(self.tree(memory=(0xd0000000,0x10000000)),0xd0000000),0)
        buf=ctypes.create_string_buffer(4096)
        lo=ctypes.addressof(buf)-128
        data=self.tree(memory=(lo,ARENA))
        ctypes.memmove(buf,data,len(data))
        self.assertEqual(self.lib.xnu_range_allowed(buf,lo,ARENA),0)

    def test_runtime_cbmem_metadata_is_required_and_two_cell_only(self):
        for prop in ('#address-cells','#size-cells'):
            t=decode(self.tree())
            t.root.children[2].props[prop]=u32(1)
            self.assertEqual(self.allowed(t.encode()),0)
        for value in (None,b'',bytes(12)):
            t=decode(self.tree());props=t.root.children[2].children[0].props
            if value is None:del props['reg']
            else:props['reg']=value
            self.assertEqual(self.allowed(t.encode()),0)
        t=decode(self.tree())
        t.root.children[2].props.update({'#address-cells':u32(2),'#size-cells':u32(2)})
        self.assertEqual(self.allowed(t.encode()),1)
        encoded=t.encode()
        # Duplicate explicit firmware cells must not be treated as absent.
        name_offset=struct.unpack_from('>I',encoded,12)[0]
        key=encoded.find(b'#address-cells\0',name_offset)-name_offset
        token=struct.pack('>4I',3,4,key,2)
        positions=[];at=0
        while (at:=encoded.find(token,at))>=0:
            positions.append(at);at+=len(token)
        self.assertEqual(len(positions),3)  # root, reserved-memory, firmware
        at=positions[-1]
        bad=bytearray(encoded[:at]+token+encoded[at:])
        for off in (4,12,36):
            struct.pack_into('>I',bad,off,struct.unpack_from('>I',encoded,off)[0]+len(token))
        self.assertEqual(self.allowed(bad),0)

    def test_duplicate_optional_initrd_properties_are_rejected(self):
        data=self.tree(initrd=0x70000000)
        strings=struct.unpack_from('>I',data,12)[0]
        extra=b''
        for name in (b'linux,initrd-start',b'linux,initrd-end'):
            key=data.find(name+b'\0',strings)-strings
            extra+=struct.pack('>IIIQ',3,8,key,self.address)
        # Insert duplicates before chosen END_NODE, preserving all FDT bounds.
        at=strings-12
        self.assertEqual(struct.unpack_from('>I',data,at)[0],2)
        bad=bytearray(data[:at]+extra+data[at:])
        for off in (4,12,36):
            struct.pack_into('>I',bad,off,struct.unpack_from('>I',data,off)[0]+len(extra))
        self.assertEqual(self.allowed(bad),0)

    def test_cli_rejects_report_payload_collision_without_writing(self):
        with tempfile.TemporaryDirectory() as tmp:
            d=Path(tmp);data=fixture()
            (d/'kernel').write_bytes(data);(d/'adt').write_bytes(adt())
            (d/'source.json').write_text(json.dumps({'source_kernel_built':True,'prelinked':False,
                'xnu_version':'10063.141.1','source_kernel_sha256':hashlib.sha256(data).hexdigest()}))
            result=subprocess.run([sys.executable,str(ROOT/'tools/xnu_checkpoint.py'),
                '--kernel',str(d/'kernel'),'--kernel-report',str(d/'source.json'),
                '--adt',str(d/'adt'),'--output',str(d/'payload.json')],capture_output=True)
            self.assertNotEqual(result.returncode,0)
            self.assertFalse((d/'payload.json').exists())

    def test_xnu_log_requires_all_checkpoint_fields_once(self):
        fields={'entry EL':2,'current EL':1,'stage':3,'handoff EL':2,
                'XNU copied':1,'XNU checkpoint':1,'XNU boot_args verified':1,'XNU current EL':1}
        def record(items):
            text=('Trogdor-ipadOS native phase-0 probe\n'+''.join(f'{k}=0x{v:016x}\n' for k,v in items)).encode()
            return text+bytes(8192-len(text))
        self.assertEqual(read_record(record(fields.items()),True)[1],fields)
        for key in ('XNU copied','XNU checkpoint','XNU boot_args verified','XNU current EL'):
            bad=dict(fields);bad[key]=0
            with self.assertRaises(ValueError):read_record(record(bad.items()),True)
            with self.assertRaises(ValueError):read_record(record([*fields.items(),(key,1)]),True)

    def test_crosscompiled_wrapper_owns_arena_after_linux(self):
        payload,_=pack(fixture(),adt())
        image=struct.pack('<II6QII',0x91005a4d,0x14000001,0,4096,0xa,0,0,0,0x644d5241,0)+bytes(64)
        with tempfile.TemporaryDirectory() as tmp:
            wrapper,report=compile_wrapper(Path(tmp),image,4096,payload)
            self.assertEqual(report['xnu_window_offset']%0x200000,0)
            self.assertGreater(report['xnu_window_offset'],report['linux_offset']+4096)
            self.assertEqual(report['header']['memory_size'],report['xnu_window_offset']+ARENA)
            self.assertEqual(wrapper[report['linux_offset']:],image)
