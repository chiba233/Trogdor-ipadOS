#!/usr/bin/env python3
"""Run the real movable wrapper and source-XNU payload on a QEMU TCG CPU model.

Board/FDT admission, log export and Linux are stubs. This is host evidence only.
QEMU is supplied by the caller; this tool does not install or download software.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
from artifacts import extract_image, inspect_image, inspect_scalar_code
from build import CPU, elf_symbols
from read_probe import read_diagnostic_record, read_record
from relocation_audit import audit_sources

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--qemu', type=Path, required=True)
    parser.add_argument('--payload', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--load-address', type=lambda s: int(s, 0), default=0x40200000)
    args = parser.parse_args()
    qemu, payload, out = args.qemu.resolve(), args.payload.resolve(), args.output.resolve()
    if not qemu.is_file() or not payload.is_file() or out.exists() or out.is_relative_to(Path('/dev')):
        parser.error('qemu/payload must exist and output must be a new ordinary directory')
    data = payload.read_bytes()
    if len(data) < 16 or data[:8] != b'TRXNUC01':
        parser.error('unrecognized source-XNU payload')
    version = struct.unpack_from('<I', data, 8)[0]
    if version not in (6, 7, 9, 10):
        parser.error('this system regression accepts platform6, PE7 or diagnostic9/10 only')
    if args.load_address % 0x200000 or not 0x40200000 <= args.load_address <= 0x58000000:
        parser.error('load address must be 2-MiB aligned inside modeled RAM, above the QEMU DTB')
    out.mkdir(parents=True)
    # Copy the opaque input: paths cannot inject assembly source through .incbin.
    payload_name = 'payload-' + hashlib.sha256(data).hexdigest() + '.bin'
    (out / payload_name).write_bytes(data)
    (out / 'payload.S').write_text('.global LINUX_MEMORY_SIZE\n.set LINUX_MEMORY_SIZE,4096\n'
        '.section .linux,"ax"\nb host_finish\n.section .xnu_payload,"a"\n.balign 16\n'
        '.global _xnu_payload_start,_xnu_payload_end\n_xnu_payload_start:\n'
        '.incbin "' + payload_name + '"\n_xnu_payload_end:\n')
    zig = ROOT / '.tools/ziglang/ziglang/zig'
    env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(ROOT / '.tools/zig-cache'),
               ZIG_LOCAL_CACHE_DIR=str(ROOT / '.tools/zig-local-cache'))
    sources = [ROOT / 'boot' / name for name in ('xnu_checkpoint.c', 'xnu_load.c', 'xnu_tables.c',
        'xnu_cinit.c', 'xnu_cpu.c', 'xnu_platform.c', 'xnu_pe.c', 'xnu_diagnostics.c')]
    sources.append(ROOT / 'tests/qemu_probe.c')
    defines = ('PROBE_UART=0', 'PROBE_RESET_EL=0', 'PROBE_LINUX_CHAIN=1', 'PROBE_XNU_CHECKPOINT=1')
    relocations = audit_sources(zig, CPU, sources, env, out / 'relocation-audit', defines)
    elf, image = out / 'probe.elf', out / 'Image'
    subprocess.run([str(zig), 'cc', '-target', 'aarch64-freestanding-none', '-mcpu=' + CPU,
        '-mgeneral-regs-only', '-ffreestanding', '-fno-builtin', '-fno-pic', '-fno-pie',
        '-fno-stack-protector', '-fno-unwind-tables', '-fno-asynchronous-unwind-tables',
        '-Wall', '-Wextra', '-Werror', '-Os', '-g', '-nostdlib', '-static',
        *['-D' + d for d in defines], '-I' + str(ROOT / 'boot'), '-Wl,--build-id=none',
        '-Wl,--no-undefined', '-Wl,-z,max-page-size=4096', '-Wl,-T,' + str(ROOT / 'boot/xnu-chain.ld'),
        str(ROOT / 'boot/entry.S'), *map(str, sources), str(out / 'payload.S'), '-o', str(elf)],
        cwd=out, env=env, check=True)
    linked = elf.read_bytes()
    image.write_bytes(extract_image(linked))
    symbols = elf_symbols(linked)
    start, end = (symbols[name]['address'] for name in ('_xnu_payload_start', '_xnu_payload_end'))
    if image.read_bytes()[start:end] != data:
        raise ValueError('linked payload differs from the requested input')
    scalar = inspect_scalar_code(linked)
    header = inspect_image(image.read_bytes())
    command = [str(qemu), '-machine', 'virt,virtualization=on,secure=off',
        '-cpu', 'cortex-a76,cntfrq=19200000', '-accel', 'tcg,thread=single', '-m', '512M',
        '-smp', '1', '-nographic', '-monitor', 'none', '-serial', 'none',
        '-semihosting-config', 'enable=on,target=native', '-device',
        f'loader,file={image},addr={args.load_address:#x},force-raw=on,cpu-num=0',
        '-d', 'int,guest_errors', '-D', str(out / 'exceptions.trace')]
    try:
        result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=30)
    except subprocess.TimeoutExpired as error:
        (out / 'console.log').write_bytes(error.stdout or b'')
        raise RuntimeError('CPU-model execution timed out; inspect console and exception trace') from error
    log = result.stdout
    (out / 'console.log').write_bytes(log)
    if result.returncode or b'HOST_LINUX_ENTRY_REACHED\n' not in log or b'HOST_LOG_EXPORT_REACHED\n' not in log:
        raise RuntimeError('CPU-model wrapper did not reach the Linux-entry stub')
    if len(log) >= 8192:
        raise ValueError('modeled log exceeds receiver capacity')
    record = log.ljust(8192, b'\0')
    if version in (6, 7):
        _, fields = read_record(record, xnu_platform=version == 6, xnu_pe=version == 7)
    else:
        _, fields = read_diagnostic_record(record)
    if version in (9, 10) and fields['XNU payload version'] != version:
        raise ValueError('runtime diagnostic version differs from requested payload')
    if version == 9:
        read_record(record, xnu_soc=True)
    (out / 'probe-log.bin').write_bytes(record)
    report = {'evidence': 'host TCG CPU model; not hardware acceptance',
        'excluded': ['board/FDT admission', 'real log export', 'real Linux boot', 'SC7180 devices'],
        'qemu_version': subprocess.check_output([str(qemu), '--version']).decode().splitlines()[0],
        'payload_version': version, 'payload_sha256': hashlib.sha256(data).hexdigest(),
        'load_address': args.load_address, 'image': header, 'scalar_code': scalar,
        'c_relocations': relocations, 'fields': fields,
        'code_sha256': {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in [ROOT / 'boot/entry.S', ROOT / 'boot/xnu-chain.ld', *sources,
                      Path(__file__).resolve(), ROOT / 'tools/relocation_audit.py', ROOT / 'tools/read_probe.py']}}
    (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(f'payload version{version}: CPU-model exception/return checks passed; hardware unverified')


if __name__ == '__main__':
    main()
