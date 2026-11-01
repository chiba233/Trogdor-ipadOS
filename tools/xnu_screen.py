#!/usr/bin/env python3
"""Pack a source-XNU screen-output payload: no checkpoint patch, no return to Linux."""
import argparse
import hashlib
from pathlib import Path
import re
import struct
import tomllib

from xnu_checkpoint import HEADER, MAGIC, adt_fields
from xnu_init import ramdisk as init_ramdisk, trust_cache
from xnu_macho import macho

VERSION = 12
# Without prelinked components the type distribution is skewed: the default budget of
# 260 gives one size class more than the 32 zones kalloc allows.
COMMAND_LINE = '-v debug=0xc serial=3 cpus=1 keepsyms=1 -enable_kprintf_spam kt_fixed_zones=120 rd=md0 cs_enforcement_disable=1'
# Original AMFI runs in the prelinked kernel: the init program is trusted through the static trust cache.
PRELINK_COMMAND_LINE = '-v debug=0xc serial=3 cpus=1 keepsyms=1 -enable_kprintf_spam rd=md0'
# The Cryptex1,SystemOS volume of the pinned IPSW, mounted by coachz-prelink-systemos.patch.
SYSTEMOS_UUID = '8846FE78-2910-4297-AF97-D59BB6F8FC16'
OS_PROFILE = Path(__file__).resolve().parents[1] / 'upstream/unixnu/profiles/os/ipados-17.7.11-21H461/profile.toml'


def up(value):
    return (value + 0x3fff) & ~0x3fff


def add_ramdisk_property(adt, base, size):
    """Add /chosen/memory-map RAMDisk, read by IOFindBSDRoot as (physical base, size)."""
    at = 0
    def visit():
        nonlocal at
        node = at
        props, children = struct.unpack_from('<II', adt, at)
        at += 8
        name = b''
        for _ in range(props):
            key = adt[at:at + 32].split(b'\0')[0]
            length = struct.unpack_from('<I', adt, at + 32)[0] & 0x7fffffff
            if key == b'name':
                name = adt[at + 36:at + 36 + length].split(b'\0')[0]
            at = (at + 36 + length + 3) & ~3
        if name == b'memory-map':
            return node, at
        for _ in range(children):
            found = visit()
            if found:
                return found
        return None
    found = visit()
    if not found:
        raise ValueError('ADT has no memory-map node')
    node, end = found
    prop = b'RAMDisk'.ljust(32, b'\0') + struct.pack('<IQQ', 16, base, size)
    result = bytearray(adt[:end]) + prop + adt[end:]
    struct.pack_into('<I', result, node, struct.unpack_from('<I', adt, node)[0] + 1)
    return result


class Node:
    """An ADT node: ordered (key, raw length word, value) properties and children."""
    def __init__(self):
        self.props, self.children = [], []

    def name(self):
        return next((value.split(b'\0')[0].decode() for key, _, value in self.props if key == 'name'), '')

    def set(self, key, value):
        self.props = [p for p in self.props if p[0] != key] + [(key, len(value), value)]

    def child(self, name):
        for node in self.children:
            if node.name() == name:
                return node
        node = Node()
        node.set('name', name.encode() + b'\0')
        self.children.append(node)
        return node

    def write(self):
        out = struct.pack('<II', len(self.props), len(self.children))
        for key, length, value in self.props:
            out += key.encode().ljust(32, b'\0') + struct.pack('<I', length) + value + bytes(-len(value) % 4)
        return out + b''.join(node.write() for node in self.children)


def read_tree(adt):
    at = 0
    def visit():
        nonlocal at
        node = Node()
        props, children = struct.unpack_from('<II', adt, at)
        at += 8
        for _ in range(props):
            key = adt[at:at + 32].split(b'\0')[0].decode()
            length = struct.unpack_from('<I', adt, at + 32)[0]
            size = length & 0x7fffffff
            node.props.append((key, length, bytes(adt[at + 36:at + 36 + size])))
            at = (at + 36 + size + 3) & ~3
        node.children = [visit() for _ in range(children)]
        return node
    return visit()


# The Coachz has no Home button, so the display route presents a full-screen 11-inch iPad Pro (UniXNU guide/mobilegestalt).
# Root compatible stays J120AP: original platform code and this IPSW's per-model resources key on it.
FULL_SCREEN_IDENTITY = {
    'root': {'string': {'model': 'iPad8,1', 'target-type': 'J317'}},
    'product': {'string': {'product-name': 'iPad Pro (11-inch)', 'product-description': 'iPad Pro (11-inch)',
                           'compatible-device-fallback': 'iPad7,3',
                           # sRGB as the GopFramebuffer route needs (UniXNU devices/sheng): with Metal present and P3, the render server picks the b3a8
                           # surface the display driver refuses, and nothing reaches the screen.
                           'artwork-display-gamut': 'sRGB'},
                'u32': {'artwork-device-subtype': 2388, 'thin-bezel': 1}},
    'buttons': {'string': {'compatible': 'buttons', 'device_type': 'buttons'},
                'bytes': {'button-names': b'hold\0voldown\0volup\0'.hex()},
                'u32': {'home-button-type': 2}},
}


# Must stay the same across boots: ByHost preference files are named after the UUID derived from it.
COACHZ_UNIQUE_CHIP_ID = b'COACHZ\0\1'


def apply_identity(node, table):
    """Apply a typed [adt] table of the OS profile: what the original components read."""
    for kind, values in table.items():
        if kind == 'string':
            for key, value in values.items():
                node.set(key, value.encode() + b'\0')
        elif kind == 'u32':
            for key, value in values.items():
                node.set(key, struct.pack('<I', value))
        elif kind == 'bytes':
            for key, value in values.items():
                node.set(key, bytes.fromhex(value))
        else:
            apply_identity(node.child(kind), values)


def wrap_trust_cache(modules):
    """The memory-map segment: a module count and offset table in front of the original modules."""
    if isinstance(modules, bytes):
        modules = [modules]
    offsets, at = [], 4 + 4 * len(modules)
    for module in modules:
        offsets.append(at)
        at += len(module)
    return struct.pack('<%dI' % (1 + len(modules)), len(modules), *offsets) + b''.join(modules)


def root_matching(uuid):
    """/chosen/root-matching: IOFindBSDRoot takes it before rd=, matching the APFS volume media by UUID.
    rd=uuid would wait for a boot-uuid-media resource that only a file system driver kext publishes."""
    return ('<dict><key>IOProviderClass</key><string>IOMedia</string><key>IOPropertyMatch</key>'
            '<dict><key>UUID</key><string>%s</string></dict></dict>' % uuid).encode() + b'\0'


def smp_tree(root, count):
    """CPU0..count-1: SC7180 numbers its cores in MPIDR Affinity1; CPU0-5 are A55 (E), CPU6-7 A76 (P).
    PSCI is called by SMC."""
    if count <= 1:
        return
    cpus = root.child('cpus')
    boot = cpus.child('cpu0')
    boot.set('cluster-type', struct.pack('<I', ord('E')))
    for index in range(1, count):
        cpu = cpus.child('cpu%d' % index)
        for key, _, value in boot.props:
            if key not in ('name', 'reg', 'state'):
                cpu.set(key, value)
        cpu.set('reg', struct.pack('<I', index << 8))
        cpu.set('state', b'waiting\0')
        cpu.set('cluster-type', struct.pack('<I', ord('P' if index >= 6 else 'E')))
    psci = root.child('psci')
    psci.set('compatible', b'arm,psci-1.0\0')
    psci.set('method', b'smc\0')


def prelink_tree(adt, window, tree, ramdisk, cache, environment=None, ram_root=True, root_uuid=None, cpus=1,
                 full_screen=False, gmu_firmware_size=0, sqe_firmware_size=0, gpu_addon_size=0, motion_probe=False):
    root = read_tree(adt)
    if cpus > 1:
        smp_tree(root, cpus)
    table = tomllib.loads(OS_PROFILE.read_text())['adt']
    apply_identity(root, table['root'])
    for name in ('chosen', 'product', 'defaults'):
        apply_identity(root.child(name), table[name])
    if full_screen:
        apply_identity(root, FULL_SCREEN_IDENTITY['root'])
        for name in ('product', 'buttons'):
            apply_identity(root.child(name), FULL_SCREEN_IDENTITY[name])
    if environment:
        root.child('chosen').set('osenvironment', environment.encode() + b'\0')
    if root_uuid:
        root.child('chosen').set('root-matching', root_matching(root_uuid))
        # mount -P 1 of the original launchd exits 66 without it. An identity of this unsigned image set,
        # not a measurement: nothing here is verified.
        root.child('chosen').set('boot-manifest-hash', hashlib.sha384(
            b'Coachz unsealed USB boot set\0' + root_uuid.encode() + b'\0' + SYSTEMOS_UUID.encode()).digest())
        # XNU derives IOPlatformUUID only from these two (IOPlatformExpertDevice::generatePlatformUUID). Without it every
        # CurrentHost preference write fails, so data migration never records that it finished and runs again on every
        # boot, holding SpringBoard's check-in (ios-sweet NOTES.md). A fixed synthetic chip ID, not a real one; 0x8011 is A10X.
        root.child('chosen').set('unique-chip-id', COACHZ_UNIQUE_CHIP_ID)
        root.child('chosen').set('chip-id', struct.pack('<I', 0x8011))
    memory_map = root.child('chosen').child('memory-map')
    # Without a RAMDisk entry IOFindBSDRoot makes no md0, and rd= names a disk partition.
    if ram_root:
        memory_map.set('RAMDisk', bytes(16))
    memory_map.set('TrustCache', bytes(16))
    if gmu_firmware_size:
        memory_map.set('CoachzGmuFirmware', bytes(16))
    if full_screen:
        # Physical u64 base/size pairs: TM0, SROT0, TM1, SROT1. Not arm-io offsets.
        thermal = root.child('arm-io').child('coachz-thermal')
        thermal.set('compatible', b'google,coachz-tsens-v2\0')
        thermal.set('reg', struct.pack('<8Q', 0x0c263000, 0x200, 0x0c222000, 0x200, 0x0c265000, 0x200, 0x0c223000, 0x200))
        wifi = root.child('arm-io').child('coachz-wifi')
        wifi.set('compatible', b'google,coachz-wcn3990\0')
        wifi.set('smem-region', struct.pack('<QQ', 0x80900000, 0x200000))
    if motion_probe:
        # locationd looks for a registry entry named gyro before it uses motion hardware (coachz-prelink-match-log.patch saw it on
        # hardware). An otherwise empty node, matched by no driver, shows which properties it then reads.
        root.child('arm-io').child('gyro').set('compatible', b'coachz,motion-probe\0')
        # locationd (CLGyro) reads gyro-interrupt-calibration as CFData of at least 8 bytes: u16 version (nonzero) at 0, u16 dt at 4, u16 rate
        # at 8 (it picks 800 or 200 itself when rate is 0). A version 1 value moves it past that check to whatever it needs next.
        root.child('arm-io').child('gyro').set('gyro-interrupt-calibration', struct.pack('<6H', 1, 0, 1250, 0, 100, 0))
    disk = window + up(tree + len(root.write()))
    if ram_root:
        memory_map.set('RAMDisk', struct.pack('<QQ', disk, len(ramdisk)))
    memory_map.set('TrustCache', struct.pack('<QQ', disk + len(ramdisk), len(cache)))
    if gmu_firmware_size:
        # The A618 GMU firmware right after the trust cache, inside the loader's kernel data (coachz-prelink-gpu.patch, coachz-gpu=3, 6 or 7).
        memory_map.set('CoachzGmuFirmware', struct.pack('<QQ', disk + len(ramdisk) + len(cache) + (-len(cache) % 4096),
                                                        gmu_firmware_size))
    if sqe_firmware_size:
        # The SQE firmware after the GMU's, for coachz-gpu=8.
        memory_map.set('CoachzSqeFirmware', struct.pack('<QQ', disk + len(ramdisk) + len(cache) + (-len(cache) % 4096) + gmu_firmware_size + (-gmu_firmware_size % 4096),
                                                        sqe_firmware_size))
    if gpu_addon_size:
        # The Metal plug-in after the firmwares, for coachz-gpu=9 (tools/xnu_gpu_userland.py); the kernel writes it to the Data volume.
        memory_map.set('GpuAddon', struct.pack('<QQ', disk + len(ramdisk) + len(cache) + (-len(cache) % 4096) + gmu_firmware_size + (-gmu_firmware_size % 4096)
                                               + sqe_firmware_size + (-sqe_firmware_size % 4096), gpu_addon_size))
    return root.write()


def pack(data, adt, window, memory, command_line=None, ramdisk=None, prelinked=False, root_cache=None, root_device=None,
         root_uuid=None, data_seed=None, data_uuid=None, display=False, cpus=1, gpu=0, gmu_firmware=None, sqe_firmware=None, gpu_addon=None,
         diagnostics=False):
    if window & 0x1fffff or memory & 0x1fffff:
        raise ValueError('window base and size must be 2 MiB aligned')
    kernel = macho(data)
    disk_root = root_device or root_uuid
    if disk_root and (not prelinked or not root_cache or ramdisk is not None or command_line or (root_device and root_uuid)):
        raise ValueError('a disk root needs the prelinked kernel, its trust cache and no RAM disk')
    command_line = command_line or (PRELINK_COMMAND_LINE if prelinked else COMMAND_LINE)
    if root_device:
        if not re.fullmatch(r'disk[0-9]+(s[0-9]+)*', root_device):
            raise ValueError('invalid root device name')
        command_line = command_line.replace('rd=md0', 'rd=' + root_device)
    if root_uuid:
        # The main OS volume by its APFS volume UUID: container numbering does not matter.
        if not re.fullmatch(r'[0-9A-F]{8}(-[0-9A-F]{4}){3}-[0-9A-F]{12}', root_uuid):
            raise ValueError('invalid root volume UUID')
        command_line = command_line.replace(' rd=md0', '')
        # Compose all SystemOS files at their normal paths, not only the shared-cache directory.
        command_line += ' coachz-systemos-union=1'
    if diagnostics and not display:
        raise ValueError('diagnostics go with the native display route')
    if display:
        if not root_uuid or not prelinked:
            raise ValueError('the native display route needs the prelinked main OS')
        command_line += ' coachz-display=1 offline-legacy-register=1 offline-legacy-gop=1 offline-legacy-phase=4'
        command_line += ' offline-legacy-pm=1 offline-legacy-validate=1 offline-legacy-transfer=1 offline-legacy-cpu-present=1'
        command_line += ' sheng-vsync=60 sheng-early-retire=0 coachz-reset-mgcache=1'
        # The first frame silences the screen console until a panic (CoachzDisplay.cpp); the disk log keeps its output.
        command_line += ' coachz-console-band=0'
        if diagnostics:
            # Measurement boots: the GPU stack's frame statistics (the plug-in's sheng-diag=1), and the rebuilt MobileGestalt cache in hex to the
            # log ring (coachz-prelink-mgcache-reset.patch), for the screen answers the system got, and the IOKit queries of locationd,
            # backboardd and SpringBoard (coachz-prelink-match-log.patch), for what CoreMotion looks for.
            command_line += ' sheng-diag=1 coachz-dump-mgcache=1 coachz-match-log=1'
        # The GT7375P touchscreen on SE4 as an IOHIDDevice digitizer (coachz-prelink-touch.patch, build-sheng-hid-input.py).
        command_line += ' coachz-touch=1'
        # UniXNU's AppleKeyStoreState: no keybag, no passcode, nothing locked (package/CoachzKeyStore.cpp).
        command_line += ' coachz-aks=1'
        # os_log of SpringBoard, runningboardd and the first apps goes to the disk log (coachz-prelink-oslog.patch).
        command_line += ' coachz-oslog=1'
        # WebKit's extension processes fail AMFI's launch constraints here (coachz-prelink-crash.patch).
        command_line += ' coachz-no-launch-constraints=1'
        # CoreImage calls an empty OpenGL table in iconservicesagent, which Settings waits on (coachz-prelink-software-render.patch).
        command_line += ' coachz-null-call=1'
        # The drivers under test: Chrome EC battery, a speaker tone and the volume buttons (docs/plans/battery-speaker.md, input.md).
        # coachz-speaker=2 starts the audio service; its plug-in's trust cache module comes with --root-trust-cache (build-coachz-audio.py).
        command_line += ' coachz-battery=1 coachz-speaker=2 coachz-buttons=1'
        # The panel backlight behind the system's brightness control, and the Zed keyboard (docs/plans/backlight.md, input.md).
        command_line += ' coachz-backlight=1 coachz-usb-keyboard=1'
        # Read-only probes: SoC temperatures and the SMEM inventory, with their nodes from prelink_tree (docs/plans/thermal.md, wifi.md).
        # coachz-wifi=2 adds the AOSS channel and one RPMh vote to the read-only MPSS inventory, 3 the first part of the MBA start, undone after
        # 2 s, 4 the whole MBA start (Q6 boot FSM, PBL, MBA) at 200 s followed by Linux's reclaim (coachz-prelink-wifi.patch).
        command_line += ' coachz-thermal=1 coachz-wifi=4 coachz-motion=1 coachz-switches=1'
        # The MPSS firmware tools/linux_log.py --modem-firmware stores in the kernel partition, read back and hashed (nothing is started).
        command_line += ' coachz-modem-fw=1'
    if gpu:
        # Adreno 618 bring-up stages (docs/plans/gpu.md): 1 inventory, 2 CX + GMU clock window, 3 also the GMU firmware into TCM,
        # 4 the window with a read-only GPU SMMU inventory.
        if not prelinked or gpu not in (1, 2, 3, 4, 5, 6, 7, 8, 9) or (gpu in (3, 6, 7, 8, 9)) != (gmu_firmware is not None) or (gpu in (8, 9)) != (sqe_firmware is not None) or (gpu == 9) != (gpu_addon is not None):
            raise ValueError('GPU stage 1 to 4 on the prelinked kernel; stage 3, and only it, takes the GMU firmware')
        if sqe_firmware is not None and not (16 <= len(sqe_firmware) <= 0x10004 and len(sqe_firmware) % 4 == 0):
            raise ValueError('The SQE firmware must be whole words and fit the 64 KiB the GPU pool has for it')
        if gmu_firmware is not None and not (4 <= len(gmu_firmware) <= 0x8000 and len(gmu_firmware) % 4 == 0):
            raise ValueError('the legacy GMU firmware fills at most the 32 KiB of ITCM and DTCM, in whole words')
        command_line += ' coachz-gpu=%d' % gpu
        if gpu == 9:
            # GPU_SET mode of the display driver: the compositor's frames reach the scanout through the GPU, not by a CPU copy.
            command_line += ' sheng-fb-gpu=1'
            # Every pass in system memory (the plug-in sets TU_DEBUG from it). Turnip's own tile-memory choice (k82) halved SP busy and cut the
            # UCHE latency by a quarter on hardware, but commit to complete did not fall and every colour came out with red and blue swapped.
            command_line += ' sheng-tu-debug=sysmem,nocb'
            # The temperature clamp (coachz-prelink-thermal.patch). The busy-driven GPU governor (coachz-gpu-dcvs=1, CoachzGpu.cpp) is left out
            # while the DDR vote is measured, so the GPU stays at 800 MHz on both sides of it.
            command_line += ' coachz-thermal-clamp=1'
            # The DDR and memory NoC bandwidth mainline votes for the top CPU and GPU states, at start (coachz_wifi.c): without it the
            # memory ran at about two thirds of Debian's speed. The render server's frame trace is summarised per 30 s (coachz_trace.c).
            command_line += ' coachz-ddr-vote=1 coachz-gpu-trace=1'
            # The system cache as mainline sets it up: sc7180's slices configured, CPUSS active, and the GPU and GPUHTW slices for the GPU (CoachzGpu.cpp).
            command_line += ' coachz-llcc=1'
    if cpus != 1:
        # coachz-prelink-smp.patch raises BCM2837's board configuration to eight CPUs in two clusters.
        if not root_uuid or not prelinked or not 2 <= cpus <= 8:
            raise ValueError('secondary CPUs need the prelinked main OS and 2 to 8 CPUs')
        if command_line.count(' cpus=1 ') != 1:
            raise ValueError('the command line must limit XNU to the boot CPU before SMP widens it')
        # cpus=N is the topology XNU parses; a CPU beyond it gets no logical number and ArmCpu skips it.
        command_line = command_line.replace(' cpus=1 ', ' cpus=%d ' % cpus)
        # coachz-max-freq=1: ArmCpu raises the A55, A76 and L3 domains to their top non-boost states.
        command_line += ' sheng-smp=1 armcpu-self-ipi=1 armcpu-el2-entry=1 coachz-max-freq=1'
    if data_uuid:
        if not root_uuid or data_seed is not None or not re.fullmatch(r'[0-9A-F]{8}(-[0-9A-F]{4}){3}-[0-9A-F]{12}', data_uuid):
            raise ValueError('an external Data UUID needs the main OS root and no memory Data seed')
        command_line += ' coachz-data-uuid=' + data_uuid
        if display:
            # Persistent Data shown on screen: skip Setup and migration, no BackBoard watchdogs (coachz-prelink-skip-setup.patch).
            command_line += ' coachz-skip-setup=1'
    if data_seed is not None and not root_uuid:
        raise ValueError('a Data seed goes with the main OS root')
    if data_seed or data_uuid:
        # The real board has no IOWatchdog service yet; the override lives on the selected Data volume.
        command_line += ' coachz-disable-watchdogd=1'
    if disk_root:
        # The main OS can carry its Data seed as the RAM disk: md0, mounted on /private/var by the kernel.
        ramdisk = data_seed or b''
    ramdisk = init_ramdisk() if ramdisk is None else ramdisk
    span = kernel.end - kernel.base
    bias = kernel.base & 0x1fffff
    # Same placement as boot/xnu_screen.c: kernel, boot_args page, ADT, RAM disk.
    tree = up(span + bias) + 4096
    if prelinked:
        cache = wrap_trust_cache(root_cache) if root_cache else trust_cache(ramdisk)
        ramdisk += bytes(-len(ramdisk) % 4096)
        # The restore volume, from memory or from disk, runs as the restore OS environment; the main OS does not.
        environment = 'restore' if root_cache and not root_uuid else None
        adt = prelink_tree(adt, window, tree, ramdisk, cache, environment, ram_root=not disk_root or bool(data_seed),
                           root_uuid=root_uuid, cpus=cpus, full_screen=display,
                           gmu_firmware_size=len(gmu_firmware) if gmu_firmware else 0, sqe_firmware_size=len(sqe_firmware) if sqe_firmware else 0, gpu_addon_size=len(gpu_addon) if gpu_addon else 0, motion_probe=diagnostics)
        ramdisk = ramdisk + cache + bytes(-len(cache) % 4096)
        if gmu_firmware:
            ramdisk += gmu_firmware + bytes(-len(gmu_firmware) % 4096)
        if sqe_firmware:
            ramdisk += sqe_firmware + bytes(-len(sqe_firmware) % 4096)
        if gpu_addon:
            ramdisk += gpu_addon + bytes(-len(gpu_addon) % 4096)
    else:
        adt = add_ramdisk_property(adt, window + up(tree + len(adt) + 52), len(ramdisk))
    base_field, size_field, affinity, frequency = adt_fields(adt)
    adt = bytearray(adt)
    for at in (base_field, size_field):
        adt[at:at + 8] = bytes(8)
    raw = command_line.encode('ascii')
    if len(raw) >= 1024:
        raise ValueError('invalid command line')
    table = b''.join((va - kernel.base).to_bytes(8, 'little') + size.to_bytes(8, 'little') +
                     off.to_bytes(8, 'little') + length.to_bytes(8, 'little') + prot.to_bytes(8, 'little')
                     for va, size, off, length, prot in kernel.segments)
    raw_at = (HEADER.size + len(table) + 15) & ~15
    adt_at = (raw_at + len(data) + 15) & ~15
    # Aligned so that the loader can copy in 64-bit units: byte by byte takes minutes with the caches off.
    ramdisk_at = (adt_at + len(adt) + 15) & ~15
    header = HEADER.pack(MAGIC, VERSION, len(kernel.segments), kernel.base, kernel.entry, window, ramdisk_at, raw_at, len(data),
                         adt_at, len(adt), span, memory, base_field, size_field, affinity, frequency, raw)
    payload = header + table + bytes(raw_at - HEADER.size - len(table)) + data
    return payload + bytes(adt_at - len(payload)) + adt + bytes(ramdisk_at - adt_at - len(adt)) + ramdisk


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--kernel', type=Path, required=True)
    p.add_argument('--adt', type=Path, required=True)
    p.add_argument('--window', type=lambda v: int(v, 0), required=True, help='physical base; choose it from the private device tree')
    p.add_argument('--memory', type=lambda v: int(v, 0), default=1 << 30)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--prelinked', action='store_true', help='kernel carries the original components: trust the init program through a trust cache')
    p.add_argument('--root-image', type=Path, help='with --prelinked: RAM disk root instead of the init program; from tools/xnu_ipsw.py')
    p.add_argument('--root-trust-cache', type=Path, action='append', default=[],
                   help='an original trust cache module of the root; repeat for the main OS and its cryptexes')
    p.add_argument('--root-device', help='with --prelinked: root on this BSD disk instead of a RAM disk, such as disk1s1')
    p.add_argument('--root-uuid', help='with --prelinked: the main OS, rooted on the APFS volume of this UUID')
    p.add_argument('--display', action='store_true', help='main OS: publish the firmware framebuffer through original MobileFramebuffer Legacy')
    p.add_argument('--smp', type=int, default=1, help='main OS: start CPU0..N-1 (up to 8) through PSCI')
    p.add_argument('--gpu', type=int, default=0, help='prelinked: Adreno 618 bring-up stage 1 to 8, or 9 for the driver (docs/plans/gpu.md)')
    p.add_argument('--gpu-addon', type=Path, help='with --gpu 9: the Metal plug-in addon from tools/xnu_gpu_userland.py')
    p.add_argument('--sqe-firmware', type=Path, help='with --gpu 8 or 9: the CP firmware, linux-firmware qcom/a630_sqe.fw')
    p.add_argument('--gmu-firmware', type=Path, help='with --gpu 3 or 6 to 9: the legacy A618 GMU firmware, linux-firmware qcom/a630_gmu.bin')
    p.add_argument('--data-uuid', help='with --root-uuid: selected external Data APFS volume UUID')
    p.add_argument('--data-image', type=Path, help='with --root-uuid: APFS Data seed, the RAM disk the kernel mounts on /private/var')
    p.add_argument('--diagnostics', action='store_true', help='with --display: GPU frame statistics and a MobileGestalt cache dump in the log')
    a = p.parse_args()
    if a.output.exists() or a.output.resolve().is_relative_to(Path('/dev')):
        p.error('output must be a new ordinary file')
    if a.root_device or a.root_uuid:
        if bool(a.root_device) == bool(a.root_uuid) or a.root_image or not a.root_trust_cache or not a.prelinked:
            p.error('--root-device or --root-uuid takes --root-trust-cache and --prelinked, and no --root-image')
    elif bool(a.root_image) != bool(a.root_trust_cache) or (a.root_image and not a.prelinked) or len(a.root_trust_cache) > 1:
        p.error('--root-image and one --root-trust-cache go together and need --prelinked')
    payload = pack(a.kernel.read_bytes(), a.adt.read_bytes(), a.window, a.memory, prelinked=a.prelinked,
                   ramdisk=a.root_image.read_bytes() if a.root_image else None,
                   root_cache=[cache.read_bytes() for cache in a.root_trust_cache] or None, root_device=a.root_device,
                   root_uuid=a.root_uuid, data_seed=a.data_image.read_bytes() if a.data_image else None, data_uuid=a.data_uuid, display=a.display, cpus=a.smp,
                   gpu=a.gpu, gmu_firmware=a.gmu_firmware.read_bytes() if a.gmu_firmware else None,
                   sqe_firmware=a.sqe_firmware.read_bytes() if a.sqe_firmware else None,
                   gpu_addon=a.gpu_addon.read_bytes() if a.gpu_addon else None, diagnostics=a.diagnostics)
    a.output.parent.mkdir(parents=True, exist_ok=True)
    a.output.write_bytes(payload)
    print('Built screen-output XNU payload', hashlib.sha256(payload).hexdigest())


if __name__ == '__main__':
    main()
