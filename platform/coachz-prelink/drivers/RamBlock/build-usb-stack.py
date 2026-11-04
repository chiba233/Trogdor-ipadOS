#!/usr/bin/env python3
"""Compile the libpayload USB stack, coachz_usb.c and the SDHCI driver (mmc/) with the kernel's own compile records."""
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
mmc = HERE / 'mmc'
usbsoc = HERE / 'usbsoc'
modem = HERE / 'modem'
for source in sorted(stack.glob('*.c')) + [HERE / 'coachz_usb.c', HERE / 'coachz_usb_hid.c', HERE / 'coachz_zed_input.c', mmc / 'coachz_sdcc.c', usbsoc / 'coachz_usb_soc.c', modem / 'coachz_ipa_config.c', modem / 'coachz_gsi.c', HERE / 'coachz_modem.c']:
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
    own = source.name in ('coachz_modem.c', 'coachz_zed_input.c') or source.parent in (mmc, usbsoc, modem)
    if own:
        # Our own code: warnings stay on (but do not fail the archive), and no USB compatibility headers.
        command[1:1] = ['-I', str(modem if source.name == 'coachz_modem.c' else source.parent)]
    else:
        command[1:1] = ['-w', '-DCOACHZ_USB_KERNEL=1', '-I', str(stack / 'compat'), '-isystem', str(stack / 'include'), '-isystem', str(stack)]
    result = subprocess.run(command, cwd=record['directory'], capture_output=True, text=True)
    if result.returncode:
        raise SystemExit(source.name + ':\n' + result.stderr[-3500:])
    if own and result.stderr.strip():
        print(source.name + ' warnings:\n' + result.stderr[-3500:])
    objects.append(destination)
archive = out / 'libCoachzUsb.a'
archive.unlink(missing_ok=True)
subprocess.run(['xcrun', 'ar', 'rcs', archive, *objects], check=True)
# The driver subclasses IOBlockStorageDevice of the original IOStorageFamily, which prelink places
# at the addresses recorded here. Same way as UniXNU's build-software-aes-bindings.py.
driver = ROOT / 'build/ram-block'
subprocess.run(['xcrun', 'clang', '-target', 'arm64-apple-ios17.0', '-Werror', '-Wno-incompatible-sysroot', '-c',
                HERE / 'storage-original-bindings.S', '-o', driver / 'original-bindings.o'], check=True)
# The SDHCI driver's block devices subclass the same IOBlockStorageDevice: compiled with NativeRamBlock's own command
# (build-arm-driver.py RamBlock wrote it), only the source and the object changed.
ram_block_command = json.loads((driver / 'compile-command.json').read_text())
iokit_record = json.loads((ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json')
                          .read_text().strip().rstrip(','))
# The cellular bridge (pending/baseband) the same way: it subclasses IOService and IOUserClient and is pulled in by coachz_wifi.c and kern_exec.c.
for source in (mmc / 'CoachzMmcBlock.cpp', HERE / 'baseband/coachz_baseband.cpp'):
    command = [str(source) if value.endswith('NativeRamBlock.cpp') else
               str(driver / (source.stem + '.o')) if value.endswith('NativeRamBlock.o') else value for value in ram_block_command]
    if command == ram_block_command or str(source) not in command:
        raise SystemExit('NativeRamBlock compile command did not name its source and object')
    result = subprocess.run(command + ['-I', str(source.parent)], cwd=iokit_record['directory'], capture_output=True, text=True)
    if result.returncode:
        raise SystemExit(source.name + ':\n' + (result.stdout + result.stderr)[-3500:])
(driver / 'libRamBlock.a').unlink(missing_ok=True)
subprocess.run(['xcrun', 'ar', 'rcs', driver / 'libRamBlock.a', driver / 'NativeRamBlock.o', driver / 'CoachzMmcBlock.o', driver / 'coachz_baseband.o',
                driver / 'original-bindings.o'], check=True)
print(len(objects), 'USB stack objects archived')
