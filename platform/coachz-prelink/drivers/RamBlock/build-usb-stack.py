#!/usr/bin/env python3
"""Compile the libpayload USB stack and coachz_usb.c with the kernel's own compile record."""
import json
from pathlib import Path
import subprocess

HERE = Path(__file__).resolve().parent
ROOT = Path.cwd()
record = json.loads((ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/bsd/DEVELOPMENT/vfs_conf.o.json').read_text().strip().rstrip(','))
out = ROOT / 'build/coachz-usb'
out.mkdir(parents=True, exist_ok=True)
objects = []
stack = HERE / 'usb'
for source in sorted(stack.glob('*.c')) + [HERE / 'coachz_usb.c']:
    destination = out / (source.stem + '.o')
    command, index, arguments = [], 0, record['arguments']
    while index < len(arguments):
        value = arguments[index]
        if value == record['file']:
            command.append(str(source))
        elif value == '-o':
            command += ['-o', str(destination)]
            index += 1
        elif value in ('-MF', '-MJ'):
            index += 1
        elif value not in ('-MD', '-MP') and not value.startswith('-DMOCKFS') and value != '-Werror':
            command.append(value)
        index += 1
    command[1:1] = ['-w', '-DCOACHZ_USB_KERNEL=1', '-I', str(stack / 'compat'), '-isystem', str(stack / 'include'), '-isystem', str(stack)]
    result = subprocess.run(command, cwd=record['directory'], capture_output=True, text=True)
    if result.returncode:
        raise SystemExit(source.name + ':\n' + result.stderr[-3500:])
    objects.append(destination)
archive = out / 'libCoachzUsb.a'
archive.unlink(missing_ok=True)
subprocess.run(['xcrun', 'ar', 'rcs', archive, *objects], check=True)
# The driver subclasses IOBlockStorageDevice of the original IOStorageFamily, which prelink places
# at the addresses recorded here. Same way as UniXNU's build-software-aes-bindings.py.
driver = ROOT / 'build/ram-block'
subprocess.run(['xcrun', 'clang', '-target', 'arm64-apple-ios17.0', '-Werror', '-Wno-incompatible-sysroot', '-c',
                HERE / 'storage-original-bindings.S', '-o', driver / 'original-bindings.o'], check=True)
(driver / 'libRamBlock.a').unlink(missing_ok=True)
subprocess.run(['xcrun', 'ar', 'rcs', driver / 'libRamBlock.a', driver / 'NativeRamBlock.o', driver / 'original-bindings.o'], check=True)
print(len(objects), 'USB stack objects archived')
