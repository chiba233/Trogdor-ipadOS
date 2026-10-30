import ctypes as C
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'))
from linux_log import compile_wrapper
from read_display import FIXED,METADATA,REGISTERS,FLAGS,read_display_record
from read_probe import read_record
from fdt import Node,encode,string,u32

class Range(C.Structure): _fields_=[('base',C.c_uint64),('size',C.c_uint64),('type',C.c_uint32)]
class Info(C.Structure):
    _fields_=[('physical',C.c_uint64)]+[(k,C.c_uint32) for k in ('width','height','stride','bpp','orientation','bl_port','bl_polarity','bl_value','ranges','table_bytes','table_entries')]+[('memory',Range*64)]
class State(C.Structure): _fields_=[('span',C.c_uint64),('allocation',C.c_uint64)]+[(k,C.c_uint32) for k in ('consistent','conflict','ram')]

def checksum(data):
    padded=data+bytes(len(data)%2);total=sum(struct.unpack('<'+'H'*(len(padded)//2),padded))
    while total>>16:total=(total&65535)+(total>>16)
    return ~total&65535

def record(tag,data):return struct.pack('<II',tag,8+len(data))+data

def records(width=640,height=480,stride=2560,pa=0,port=0x3d0c000,polarity=1,memory=None):
    if memory is None:memory=[(0x80000000,0x70000000,1),(0xffc00000,0x400000,16)]
    return [record(1,b''.join(struct.pack('<QQI',*r) for r in memory)),
        struct.pack('<IIQIII10B2x',0x12,40,pa,width,height,stride,32,16,8,8,8,0,8,24,8,0),
        record(0x13,struct.pack('<I',1)+struct.pack('<III16s',port,polarity,0,b'backlight'))]

def table(items):
    body=b''.join(items);h=bytearray(struct.pack('<4s5I',b'LBIO',24,0,len(body),checksum(body),len(items)))
    struct.pack_into('<I',h,8,checksum(h));return bytes(h)+body

def log(values):
    text='Trogdor-ipadOS native phase-0 probe\n'+''.join(k+f'=0x{v:016x}\n' for k,v in {'entry EL':2,'current EL':1,'stage':3,'handoff EL':2}.items())
    text+=''.join('Display '+k+f'=0x{v:016x}\n' for k,v in values.items())
    b=text.encode();return b+bytes(8192-len(b))

def values():
    d=dict(FIXED)
    d.update(zip(METADATA,(144,3,0,640,480,2560,32,0,0x80000000,0x2000000,1228800,1228800)))
    d.update(zip(REGISTERS,(0,0,640|(480<<16),640|(480<<16),0,0,0xa0000000,2560,0x236ff,0x03020001,0,0,0x300)))
    d.update(zip(FLAGS,(1,1,1,0,1)));return d

class DisplayTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp=tempfile.TemporaryDirectory();path=Path(cls.temp.name)/'display.so'
        subprocess.run(['cc','-shared','-fPIC','-std=c11','-Wall','-Wextra','-Werror','-I'+str(ROOT/'boot'),str(ROOT/'boot/coreboot.c'),str(ROOT/'boot/display_state.c'),'-o',str(path)],check=True)
        cls.lib=C.CDLL(str(path));cls.lib.cb_display_parse.argtypes=[C.c_void_p,C.c_size_t,C.c_uint64,C.c_uint64,C.c_size_t,C.POINTER(Info)]
        cls.lib.display_analyze.argtypes=[C.POINTER(Info),C.POINTER(C.c_uint32),C.c_uint64,C.c_uint64,C.POINTER(State)]
        wrapper=Path(cls.temp.name)/'runtime.c'
        wrapper.write_text('#define cb_display_parse fixture_parse\n#include "'+str(ROOT/'boot/display.c')+'"\n#undef cb_display_parse\n#include <string.h>\nstatic struct cb_display fixture;\nstatic uint32_t rr[13];static int fault_at,parse_count,read_count,count;\nstatic char labels[64][64];static uint64_t output[64];\nint fixture_parse(const uint8_t *p,size_t n,uint64_t pa,uint64_t first,size_t limit,struct cb_display *out) {\n (void)p;parse_count++;if(n!=0x10000 || pa!=0xffc00000 || first!=pa || limit<24)return 0;*out=fixture;return 1;\n}\nint display_read32(uint64_t a,uint32_t *out) {\n const uint64_t expected[13]={0xae6b800,0xae94004,0xae05000,0xae0500c,0xae05008,0xae05010,0xae05014,0xae05024,0xae05030,0xae05034,0x3d0c000,0x3d0c004,0xa80068};\n int i=read_count++;if(i>=13 || a!=expected[i] || i==fault_at)return 0;*out=rr[i];return 1;\n}\nvoid probe_value(const char *key,uint64_t value) {if(count>=64)return;size_t n=strlen(key);if(n>=64)return;memcpy(labels[count],key,n+1);output[count++]=value;}\nvoid fixture_setup(const struct cb_display *b,const uint32_t *r,int fault) {fixture=*b;for(unsigned i=0;i<13;i++)rr[i]=r[i];fault_at=fault;}\nint fixture_run(const void *dt,uint64_t image,uint64_t size) {parse_count=read_count=count=0;display_discover(dt,image,size);return count;}\nint fixture_reads(void) {return read_count;}int fixture_parses(void) {return parse_count;}\nconst char *fixture_key(int i){return labels[i];}uint64_t fixture_value(int i){return output[i];}\n')
        path=Path(cls.temp.name)/'runtime.so'
        subprocess.run(['cc','-shared','-fPIC','-std=c11','-Wall','-Wextra','-Werror','-I'+str(ROOT/'boot'),str(wrapper),str(ROOT/'boot/coreboot.c'),str(ROOT/'boot/display_state.c'),str(ROOT/'boot/fdt.c'),str(ROOT/'boot/xnu_memory.c'),'-o',str(path)],check=True)
        cls.runtime=C.CDLL(str(path));cls.runtime.fixture_setup.argtypes=[C.POINTER(Info),C.POINTER(C.c_uint32),C.c_int]
        cls.runtime.fixture_run.argtypes=[C.c_void_p,C.c_uint64,C.c_uint64];cls.runtime.fixture_key.restype=C.c_char_p;cls.runtime.fixture_value.restype=C.c_uint64
    @classmethod
    def tearDownClass(cls):cls.temp.cleanup()
    def parse(self,data,expected=True,pa=0xffc00000,first=None,limit=None):
        out=Info();C.memset(C.byref(out),0xa5,C.sizeof(out));before=bytes(out)
        buf=C.create_string_buffer(data);ok=self.lib.cb_display_parse(buf,len(data),pa,pa if first is None else first,len(data) if limit is None else limit,C.byref(out))
        self.assertEqual(bool(ok),expected)
        if not ok:self.assertEqual(bytes(out),before)
        return out
    def test_exact_table_with_trailing_capacity_and_zero_or_nonzero_pa(self):
        for pa in (0,0xa0000000):
            d=table(records(pa=pa));out=self.parse(d+bytes(8192-len(d)),limit=len(d))
            self.assertEqual((out.physical,out.width,out.height,out.stride,out.bl_port,out.ranges),(pa,640,480,2560,0x3d0c000,2))
        d=table(records()+[record(0x1234,b'ignored!')]);self.parse(d)
    def test_every_truncation_and_checksum_corruption_preserves_output(self):
        d=table(records())
        for n in range(len(d)):self.parse(d[:n],False)
        for at in range(len(d)):
            b=bytearray(d);b[at]^=1;self.parse(bytes(b),False)
    def test_structural_duplicates_geometry_gpio_and_memory_rejections(self):
        fixtures=[records()+[records()[1]],records()+[records()[2]],records()+[records()[0]],
            records(width=0),records(width=8193),records(height=8193),records(stride=2559),records(stride=65540),
            records(port=0x1234000),records(polarity=2),records(memory=[(0x80000000,100,1),(0x80000050,100,16)]),
            records(memory=[((1<<64)-8,16,1)]),records(memory=[])]
        for rs in fixtures:self.parse(table(rs),False)
        for index,offset,value in ((1,28,24),(1,29,0),(1,37,4),(2,8,2)):
            rs=records();b=bytearray(rs[index]);b[offset]=value;rs[index]=bytes(b);self.parse(table(rs),False)
        rs=records();rs[0]=record(1,b'x'*20*65);self.parse(table(rs),False)
    def test_bounded_forward_chain_cycles_and_limit(self):
        pa=0xffc00000;target=table(records());link=table([record(0x11,struct.pack('<Q',pa+256))])
        data=link+bytes(256-len(link))+target
        self.parse(data,limit=len(link))
        self.parse(data,False,limit=len(link)-1)
        for target_pa in (pa-1,pa+len(data),pa,1<<63):
            bad=table([record(0x11,struct.pack('<Q',target_pa))]);self.parse(bad+bytes(256-len(bad))+target,False,limit=len(bad))
        loop=link+bytes(256-len(link))+table([record(0x11,struct.pack('<Q',pa))]);self.parse(loop,False,limit=len(link))
        self.parse(table([record(0x11,struct.pack('<Q',pa))]+records()),False)
    def test_snapshot_relations_and_allocation_tail_collision(self):
        b=self.parse(table(records()));r=(C.c_uint32*13)(*[values()[k] for k in REGISTERS]);o=State()
        self.assertEqual(self.lib.display_analyze(C.byref(b),r,0x80000000,0x2000000,C.byref(o)),1)
        self.assertEqual((o.consistent,o.conflict,o.ram),(1,0,1))
        for i in range(2,10):
            old=r[i];r[i]^=1;self.lib.display_analyze(C.byref(b),r,0x80000000,0x2000000,C.byref(o));self.assertEqual(o.consistent,0);r[i]=old
        b=self.parse(table(records(width=3,height=3,stride=16)))
        r[2]=r[3]=3|(3<<16);r[7]=16
        self.lib.display_analyze(C.byref(b),r,0xa0000000+64,4096,C.byref(o))
        self.assertEqual((o.span,o.allocation,o.conflict),(48,4096,1))
        b.memory[0].size=0x20000000+48
        self.lib.display_analyze(C.byref(b),r,0x80000000,4096,C.byref(o));self.assertEqual(o.ram,0)
    def test_strict_reader_requires_complete_unique_consistent_discovery(self):
        d=values();text,fields=read_display_record(log(d));self.assertEqual(len(fields),42)
        for key in d:
            bad=dict(d);del bad[key]
            with self.subTest(missing=key),self.assertRaises(ValueError):read_display_record(log(bad))
            raw=log(d);line='Display '+key+f'=0x{d[key]:016x}\n';changed=raw.split(b'\0',1)[0]+line.encode()
            with self.subTest(duplicate=key),self.assertRaises(ValueError):read_display_record(changed.ljust(8192,b'\0'))
        for k,v in [('schema',2),('hardware writes',1),('MMIO fault',1),('MMIO reads',12),('discovery complete',0),('frame span',1),('allocation bytes',1),('scanout consistent',0),('Image conflict',1),('candidate writable',0),('bpp',24),('width',0),('orientation',4),('TE',1<<32),('Image PA',1<<63)]:
            bad=dict(d);bad[k]=v
            with self.subTest(key=k),self.assertRaises(ValueError):read_display_record(log(bad))
        with self.assertRaises(ValueError):read_record(log(d),xnu_pe=True)
        bad=log(d).split(b'\0',1)[0]+b'XNU checkpoint=0x0000000000000001\n'
        with self.assertRaises(ValueError):read_display_record(bad.ljust(8192,b'\0'))
    def test_complete_discovery_can_report_ineligible_scanout(self):
        for key in ('Coreboot RAM','FDT range allowed'):
            d=values();d[key]=0;d['candidate writable']=0;read_display_record(log(d))
        d=values();d['scanout PA']=0x80000000;d['Image conflict']=1;d['candidate writable']=0;read_display_record(log(d))
        d=values();d['source size']^=1;d['scanout consistent']=d['candidate writable']=0;read_display_record(log(d))
    def test_native_discovery_build_is_separate_and_audits_all_objects(self):
        image=struct.pack('<II6QII',0x91005a4d,0x14000001,0,4096,0xa,0,0,0,0x644d5241,0)+bytes(64)
        with tempfile.TemporaryDirectory() as tmp:
            data,report=compile_wrapper(Path(tmp),image,4096,discovery=True)
            self.assertEqual(report['display_discovery_schema'],1)
            self.assertEqual(len(report['c_relocations']),7)
            self.assertEqual(data[report['linux_offset']:],image)
            self.assertEqual(data.count(struct.pack('<I',0xb9400001)),1)
            self.assertNotIn('xnu_checkpoint_version',report)
        with self.assertRaises(ValueError):compile_wrapper(Path('/tmp'),image,4096,checkpoint=b'x',discovery=True)

    def runtime_tree(self):
        return Node('',{'#address-cells':u32(2),'#size-cells':u32(2),'compatible':string('google,coachz-rev1-sku0')+string('qcom,sc7180')},[
            Node('memory',{'reg':struct.pack('>QQ',0x80000000,0x70000000)}),
            Node('firmware',{'ranges':b''},[Node('coreboot',{'reg':struct.pack('>4Q',0xffc00000,984,0xffc00000,0x10000)})]),
            Node('reserved-memory',{'#address-cells':u32(2),'#size-cells':u32(2),'ranges':b''})])
    def run_runtime(self,tree,fault=-1):
        b=self.parse(table(records()));r=(C.c_uint32*13)(*[values()[k] for k in REGISTERS]);self.runtime.fixture_setup(C.byref(b),r,fault)
        blob=C.create_string_buffer(encode(tree));n=self.runtime.fixture_run(blob,0x80000000,0x2000000)
        d={self.runtime.fixture_key(i).decode()[8:-1]:self.runtime.fixture_value(i) for i in range(n)}
        return d,self.runtime.fixture_parses(),self.runtime.fixture_reads()
    def test_runtime_fdt_provenance_rejects_before_table_or_mmio(self):
        cases=[]
        for key,value in (('compatible',string('qcom,sc7180')),('#address-cells',u32(1)),('#size-cells',u32(1))):
            t=self.runtime_tree();t.props[key]=value;cases.append(t)
        for key,value in (('ranges',b'bad!'),('#size-cells',u32(1)),('#address-cells',u32(1))):
            t=self.runtime_tree();t.children[1].props[key]=value;cases.append(t)
        for reg in ((0xffc00000,984,0xffc00000,0x300000),(0xffb00000,984,0xffc00000,0x10000),(0xffc00000,23,0xffc00000,0x10000),(0x80000000,984,0x80000000,0x10000),(0xffc00000,984,0xfffff000,0x10000)):
            t=self.runtime_tree();t.children[1].children[0].props['reg']=struct.pack('>4Q',*reg);cases.append(t)
        t=self.runtime_tree();t.children[1].children.append(Node('coreboot'));cases.append(t)
        t=self.runtime_tree();t.children.append(Node('firmware'));cases.append(t)
        for t in cases:
            d,parses,reads=self.run_runtime(t);self.assertEqual((parses,reads,d['discovery complete']),(0,0,0))
    def test_runtime_full_log_partial_fault_and_fdt_reserved_rejection(self):
        d,parses,reads=self.run_runtime(self.runtime_tree());self.assertEqual((parses,reads),(1,13));read_display_record(log(d));self.assertEqual(d['candidate writable'],1)
        for fault in range(13):
            d,parses,reads=self.run_runtime(self.runtime_tree(),fault)
            self.assertEqual((parses,reads,d['MMIO reads'],d['MMIO fault'],d['discovery complete'],d['candidate writable']),(1,fault+1,fault,1,0,0))
            with self.assertRaises(ValueError):read_display_record(log(d))
        t=self.runtime_tree();t.children[2].children.append(Node('reserved',{'reg':struct.pack('>QQ',0xa0000000,0x200000)}))
        d,_,_=self.run_runtime(t);self.assertEqual((d['FDT range allowed'],d['candidate writable']),(0,0));read_display_record(log(d))

    def test_diagnostic_reader_rejects_mixed_display_schema(self):
        from test_xnu_diagnostics import XnuDiagnosticsTests
        from read_probe import read_diagnostic_record
        fixture=XnuDiagnosticsTests();data=fixture.encode(fixture.record(1).items())
        read_diagnostic_record(data)
        text=data.split(b'\0',1)[0]+b'Display schema=0x0000000000000001\n'
        with self.assertRaises(ValueError):read_diagnostic_record(text.ljust(8192,b'\0'))
