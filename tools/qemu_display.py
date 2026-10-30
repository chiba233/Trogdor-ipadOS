#!/usr/bin/env python3
"""Exercise the actual display read/abort primitive on a TCG CPU; not hardware."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
from artifacts import extract_image,inspect_image,inspect_scalar_code
from build import CPU
from relocation_audit import audit_sources
ROOT=Path(__file__).resolve().parents[1]
def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--qemu',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
    a=p.parse_args();out=a.output.resolve();qemu=a.qemu.resolve()
    if out.exists() or out.is_relative_to(Path('/dev')) or not qemu.is_file():p.error('requires existing QEMU and a new ordinary output directory')
    out.mkdir(parents=True);zig=ROOT/'.tools/ziglang/ziglang/zig'
    env=dict(os.environ,ZIG_GLOBAL_CACHE_DIR=str(ROOT/'.tools/zig-cache'),ZIG_LOCAL_CACHE_DIR=str(ROOT/'.tools/zig-local-cache'))
    cases=[(0,0x40200000),(0,0x48200000)]+[(i,0x40200000) for i in range(1,10)];reports=[]
    code=['boot/entry.S','boot/chain.ld','tests/qemu_display.c','tests/qemu_display.S','tools/qemu_display.py','tools/build.py','tools/artifacts.py','tools/relocation_audit.py']
    for index,(case,address) in enumerate(cases):
        folder=out/str(index);folder.mkdir();(folder/'payload.S').write_text('.global LINUX_MEMORY_SIZE\n.set LINUX_MEMORY_SIZE,4096\n.section .linux,"ax"\nb host_finish\n')
        defines=('PROBE_LINUX_CHAIN=1','PROBE_XNU_CHECKPOINT=0','PROBE_DISPLAY_DISCOVERY=1','HOST_GUARD_CASE='+str(case))
        audit=audit_sources(zig,CPU,[ROOT/'tests/qemu_display.c'],env,folder/'relocation-audit',defines)
        elf=folder/'probe.elf';image=folder/'Image'
        subprocess.run([str(zig),'cc','-target','aarch64-freestanding-none','-mcpu='+CPU,'-mgeneral-regs-only','-ffreestanding','-fno-builtin','-fno-pic','-fno-pie','-fno-stack-protector','-fno-unwind-tables','-fno-asynchronous-unwind-tables','-Wall','-Wextra','-Werror','-Os','-g','-nostdlib','-static',*['-D'+d for d in defines],'-Wl,--build-id=none','-Wl,--no-undefined','-Wl,-z,max-page-size=4096','-Wl,-T,'+str(ROOT/'boot/chain.ld'),str(ROOT/'boot/entry.S'),str(ROOT/'tests/qemu_display.c'),str(ROOT/'tests/qemu_display.S'),str(folder/'payload.S'),'-o',str(elf)],env=env,check=True)
        linked=elf.read_bytes();raw=extract_image(linked);image.write_bytes(raw);scalar=inspect_scalar_code(linked)
        cmd=[str(qemu),'-machine','virt,virtualization=on,secure=off','-cpu','cortex-a76','-accel','tcg,thread=single','-m','512M','-smp','1','-nographic','-monitor','none','-serial','none','-semihosting-config','enable=on,target=native','-device',f'loader,file={image},addr={address:#x},force-raw=on,cpu-num=0','-d','int,guest_errors','-D',str(folder/'exceptions.trace')]
        result=subprocess.run(cmd,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=10);(folder/'console.log').write_bytes(result.stdout)
        if result.returncode or b'DISPLAY_MODEL_PASS\n' not in result.stdout or (case==0 and b'DISPLAY_MODEL_HVC_RETURN\n' not in result.stdout):raise RuntimeError('display read/guard CPU model failed: '+str(index))
        reports.append({'guard_case':case,'load_address':address,'image_sha256':hashlib.sha256(raw).hexdigest(),'image':inspect_image(raw),'scalar_code':scalar,'c_relocations':audit})
        print('CPU-model case',index,'passed',flush=True)
    report={'evidence':'host TCG CPU model only','excluded':['Coreboot parser/FDT admission','SC7180 MMIO','real Linux/log export','framebuffer/display/EC operation'],'cases':reports,'code_sha256':{n:hashlib.sha256((ROOT/n).read_bytes()).hexdigest() for n in code}}
    (out/'report.json').write_text(json.dumps(report,indent=2)+'\n')
if __name__=='__main__':main()
