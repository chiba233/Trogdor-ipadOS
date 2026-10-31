"""Compile the libpayload USB stack (BSD, pinned in upstream/coreboot) for the probe.

The sources stay upstream: they are extracted into the build output with two adaptations.
The wrapper links at zero and is not rebased, so tables of function pointers are filled at
run time. The probe is built with strict alignment, which turns every access to a packed
field into single bytes, so the register structures lose __packed: MMIO needs 32-bit accesses.
Only boot/usb_compat and boot/usb_read.c are ours.
"""
import json
from pathlib import Path
import re
import subprocess

from relocation_audit import inspect_object

ROOT = Path(__file__).resolve().parents[1]
DRIVERS = ('xhci.c', 'xhci_commands.c', 'xhci_devconf.c', 'xhci_events.c', 'xhci_rh.c', 'usbmsc.c', 'usb.c',
           'generic_hub.c', 'usbhub.c', 'usb_dev.c', 'quirks.c')
LOCAL_HEADERS = ('xhci.h', 'xhci_private.h', 'generic_hub.h')
HEADERS = ('usb.h', 'usbmsc.h', 'usbdisk.h')
TABLES = {'usbhub.c': ('usb_hub_ops', 'generic_hub_init(dev, num_ports, &usb_hub_ops)', '\tif ('),
          'xhci_rh.c': ('xhci_rh_ops', 'generic_hub_init(dev, num_ports, &xhci_rh_ops)', '\t')}


def runtime_table(text, name, call, prefix):
    """Replace a constant table of function pointers by assignments in front of its one use."""
    match = re.search(r'static const generic_hub_ops_t ' + name + r' = \{\n(.*?)\};\n', text, re.S)
    if not match or text.count(prefix + call) != 1:
        raise ValueError('libpayload changed around ' + name)
    fields = re.findall(r'\.(\w+)\s*=\s*(\w+),', match.group(1))
    assigned = ''.join(f'\t{name}.{field} = {value};\n' for field, value in fields if value != 'NULL')
    text = text.replace(match.group(0), f'static generic_hub_ops_t {name};\n')
    return text.replace(prefix + call, assigned + prefix + call)


def unpacked_registers(text):
    """Their members are naturally aligned; boot/usb_read.c asserts the offsets."""
    for member in ('*capreg;', 'prs[];', '*opreg;', 'intrrs[];', '*hcrreg;'):
        if text.count('} __packed ' + member) != 1:
            raise ValueError('libpayload changed around ' + member)
        text = text.replace('} __packed ' + member, '} ' + member)
    return text


def source(path):
    spec = json.loads((ROOT / 'upstream.lock.json').read_text())['sources']['coreboot']
    return subprocess.check_output(['git', '-C', str(ROOT / 'upstream/coreboot'), 'show',
                                    spec['commit'] + ':payloads/libpayload/' + path], text=True)


def extract(tree, writable_storage=False):
    (tree / 'include/usb').mkdir(parents=True)
    for name in DRIVERS + LOCAL_HEADERS:
        text = source('drivers/usb/' + name)
        if name in TABLES:
            text = runtime_table(text, *TABLES[name])
        if name == 'xhci_private.h':
            text = unpacked_registers(text)
        if writable_storage and name == 'usbmsc.c':
            text += '''\n/* Coachz: complete APFS flushes with the original bulk-only SCSI transport. */\nint coachz_usbmsc_sync(usbdev_t *dev) {\n    const u8 command[10] = {0x35}; /* SYNCHRONIZE CACHE(10), whole LUN */\n    return execute_command(dev, cbw_direction_data_out, command, 10, NULL, 0, 0);\n}\n'''
        (tree / name).write_text(text)
    for name in HEADERS:
        (tree / 'include/usb' / name).write_text(source('include/usb/' + name))


def build(zig, cpu, env, out):
    """Returns the objects to link and their relocation reports."""
    tree = out / 'libpayload-usb'
    extract(tree)
    flags = ['-target', 'aarch64-freestanding-none', '-mcpu=' + cpu, '-mgeneral-regs-only', '-ffreestanding',
             '-fno-builtin', '-fno-pic', '-fno-pie', '-fno-stack-protector', '-fno-unwind-tables',
             '-fno-asynchronous-unwind-tables', '-Os', '-g', '-I' + str(ROOT / 'boot/usb_compat'),
             '-isystem', str(tree / 'include'), '-isystem', str(tree)]
    objects, reports = [], {}
    for path, warnings in [(tree / name, ['-w']) for name in DRIVERS] + [(ROOT / 'boot/usb_read.c', ['-Wall', '-Wextra', '-Werror'])]:
        obj = tree / (path.stem + '.o')
        subprocess.run([str(zig), 'cc', *flags, *warnings, '-c', str(path), '-o', str(obj)], env=env, check=True)
        reports[path.name] = inspect_object(obj.read_bytes())
        objects.append(obj)
    return objects, reports
