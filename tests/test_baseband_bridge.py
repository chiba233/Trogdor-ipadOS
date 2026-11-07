"""Host checks for the baseband bridge (QMUX/CTL over QRTR, SAHARA device end, AppleBaseband methods), its kernel patch and its packaging
(tools/xnu_screen.py --baseband, tools/linux_log.py --baseband). Never builds or boots a kernel."""
import glob
import hashlib
import importlib.util
import io
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
import zipfile
import zlib

ROOT = Path(__file__).resolve().parents[1]
PENDING = ROOT / 'platform/coachz-prelink/pending/baseband'
PATCHES = ROOT / 'platform/coachz-prelink/patches/xnu'


def load_screen():
    sys.path.insert(0, str(ROOT / 'tools'))
    try:
        spec = importlib.util.spec_from_file_location('xnu_screen_baseband', ROOT / 'tools/xnu_screen.py')
        screen = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(screen)
    finally:
        sys.path.remove(str(ROOT / 'tools'))
    return screen


class BasebandBridgeTests(unittest.TestCase):
    def test_portable_model(self):
        cc = shutil.which('cc')
        self.assertIsNotNone(cc)
        with tempfile.TemporaryDirectory(prefix='coachz-bb-') as directory:
            binary = Path(directory) / 'bb-model'
            subprocess.run([cc, '-std=c11', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined', '-I', str(PENDING),
                            str(ROOT / 'tests/coachz_bb_model_test.c'), '-o', str(binary)], check=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual(result.stdout.count('PASS '), 8, result.stdout)

    def test_headers_compile_as_cpp(self):
        """coachz_baseband.cpp includes the portable headers as C++."""
        cxx = shutil.which('c++')
        self.assertIsNotNone(cxx)
        with tempfile.TemporaryDirectory(prefix='coachz-bb-cpp-') as directory:
            source = Path(directory) / 'tu.cpp'
            source.write_text('#include "coachz_bb_qmux.h"\n#include "coachz_bb_sahara.h"\n#include "coachz_bb_control.h"\n#include "coachz_bb_data.h"\n'
                              '#include "coachz_bb_channels.h"\n#include "coachz_bb_esim.h"\nint main() { return 0; }\n')
            result = subprocess.run([cxx, '-std=gnu++2b', '-fsyntax-only', '-Wall', '-Werror', '-I', str(PENDING), str(source)],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_kernel_patch_context_matches_the_series(self):
        """Every context line of the patch is a line the xnu patches before it produce, and it is the last of the series."""
        patch = (PATCHES / 'coachz-prelink-baseband.patch').read_text()
        self.assertEqual(re.findall(r'^\+\+\+ b/(\S+)', patch, re.M), ['bsd/dev/coachz_wifi.c', 'bsd/kern/kern_exec.c'])
        produced = set()
        for path in glob.glob(str(PATCHES / '*.patch')):
            if Path(path).name == 'coachz-prelink-baseband.patch':
                continue
            produced.update(line[1:] for line in Path(path).read_text(errors='replace').splitlines() if line[:1] in '+ ')
        context = [line[1:] for hunk in re.split(r'^@@.*$', patch, flags=re.M)[1:] for line in hunk.splitlines()
                   if line.startswith(' ') and line.strip()]
        self.assertGreater(len(context), 50)
        self.assertEqual([line for line in context if line not in produced], [])
        series = [line for line in (PATCHES / 'series').read_text().splitlines() if line and not line.startswith('#')]
        # Only the video decoder's kern_exec.c hook (built on the baseband patch's result) follows it.
        self.assertEqual(series[-2:], ['coachz-prelink-baseband.patch', 'coachz-prelink-video-decoder.patch'])

    def test_in_the_build(self):
        self.assertIn("'pending/baseband', copy / 'baseband'", (ROOT / 'tools/kernel_source.py').read_text())
        build = (ROOT / 'platform/coachz-prelink/drivers/RamBlock/build-usb-stack.py').read_text()
        self.assertIn("HERE / 'baseband/coachz_baseband.cpp'", build)
        self.assertIn("driver / 'coachz_baseband.o'", build)

    def test_packaging(self):
        screen = load_screen()
        if not screen.OS_PROFILE.is_file():
            self.skipTest('needs the pinned UniXNU OS profile')
        root = screen.Node()
        root.set('name', b'device-tree\0')
        root.child('chosen').child('memory-map')
        window, tree, ramdisk, cache = 0x80000000, 0x2000000, bytes(0x5000), bytes(0x1234)
        adt = screen.prelink_tree(root.write(), window, tree, ramdisk, cache, gmu_firmware_size=0x8000, sqe_firmware_size=0x100,
                                  gpu_addon_size=0x3001, baseband=True)
        out = screen.read_tree(adt)
        chosen = {key: value for key, _, value in out.child('chosen').props}
        self.assertEqual(chosen['coachz-baseband'], struct.pack('<I', 1))
        node = {key: value for key, _, value in out.child('baseband').props}
        self.assertEqual((node['compatible'], node['device_type'], node['euicc-chip-id']),
                         (b'baseband,n41\0', b'baseband\0', struct.pack('<I', 2)))
        self.assertNotIn('interrupts', node)
        self.assertEqual(dict((k, v) for k, _, v in out.child('product').props)['baseband-chipset'], b'mav16\0')
        self.assertEqual(dict((k, v) for k, _, v in out.props)['compatible'], b'J121AP\0')   # HWModelStr: the telephony table's cellular model
        self.assertNotIn('CoachzBaseband', [key for key, _, _ in out.child('chosen').child('memory-map').props])
        plain = screen.read_tree(screen.prelink_tree(root.write(), window, tree, ramdisk, cache))
        self.assertNotIn('baseband', [child.name() for child in plain.children])
        with self.assertRaises(ValueError):
            screen.pack(b'', b'', 0, 0, baseband=True)   # the bridge needs the prelinked kernel

    def test_cellular_compat_branch(self):
        sys.path.insert(0, str(ROOT / 'tools'))
        try:
            import coachz_cellular_compat as compat
        finally:
            sys.path.remove(str(ROOT / 'tools'))
        image = bytearray(0x100 + len(compat.ORIGINAL_GUARD))
        struct.pack_into('<8I', image, 0, 0xfeedfacf, 0x100000c, 0, 2, 2, 96, 0, 0)
        struct.pack_into('<II16s4Q4I', image, 32, 0x19, 72, b'__TEXT', compat.GUARD_ADDRESS,
                         len(compat.ORIGINAL_GUARD), 0x100, len(compat.ORIGINAL_GUARD), 5, 5, 0, 0)
        struct.pack_into('<II16s', image, 104, 0x1b, 24, compat.SOURCE_UUID)
        image[0x100:] = compat.ORIGINAL_GUARD
        self.assertEqual(compat.guard_offset(image), 0x100)
        with self.assertRaises(ValueError):
            compat.validate_compat(image)
        at = 0x100 + compat.BRANCH_ADDRESS - compat.GUARD_ADDRESS
        image[at:at + 4] = compat.COMPAT_BRANCH
        compat.validate_compat(image)
        instruction = struct.unpack_from('<I', image, at)[0]
        self.assertEqual(instruction >> 26, 5)
        self.assertEqual(compat.BRANCH_ADDRESS + 4 * (instruction & 0x3ffffff), 0x100fa5aa0)
        image[112] ^= 1
        with self.assertRaises(ValueError):
            compat.validate_compat(image)

        # The local-signup gate uses a separate virtual address in the same pinned executable.
        # Retarget the fixture's text segment and exercise the optional eSIM edit through that mapping.
        image[112] ^= 1
        struct.pack_into('<Q', image, 56, compat.ESIM_GUARD_ADDRESS)
        image[0x100:] = compat.ESIM_ORIGINAL_GUARD
        tail = 0x100 + compat.ESIM_FLOW_GUARD_ADDRESS - compat.ESIM_GUARD_ADDRESS
        image.extend(bytes(tail + len(compat.ESIM_FLOW_GUARD) - len(image)))
        image[tail:tail + len(compat.ESIM_FLOW_GUARD)] = compat.ESIM_FLOW_GUARD
        install = 0x100 + compat.ESIM_INSTALL_GUARD_ADDRESS - compat.ESIM_GUARD_ADDRESS
        image.extend(bytes(install + len(compat.ESIM_INSTALL_GUARD) - len(image)))
        image[install:install + len(compat.ESIM_INSTALL_GUARD)] = compat.ESIM_INSTALL_GUARD
        eligibility = 0x100 + compat.ESIM_ELIGIBILITY_GUARD_ADDRESS - compat.ESIM_GUARD_ADDRESS
        image.extend(bytes(eligibility + len(compat.ESIM_ELIGIBILITY_GUARD) - len(image)))
        image[eligibility:] = compat.ESIM_ELIGIBILITY_GUARD
        struct.pack_into('<Q', image, 64, len(image) - 0x100)
        struct.pack_into('<Q', image, 80, len(image) - 0x100)
        corrupt = bytearray(image)
        corrupt[eligibility] ^= 1
        unchanged = bytes(corrupt)
        with self.assertRaises(ValueError):
            compat.patch_esim(corrupt)
        self.assertEqual(corrupt, unchanged)  # Reject every edit before changing any guarded window.
        compat.patch_esim(image)
        branch = compat.ESIM_BRANCH_ADDRESS - compat.ESIM_GUARD_ADDRESS
        expected = bytearray(compat.ESIM_ORIGINAL_GUARD)
        expected[branch:branch + 4] = struct.pack('<I', 0xd503201f)
        self.assertEqual(image[0x100:0x100 + len(expected)], expected)
        branch = tail + compat.ESIM_FLOW_ADDRESS - compat.ESIM_FLOW_GUARD_ADDRESS
        self.assertEqual(image[branch:branch + 4], struct.pack('<I', 0x2a0803e0))
        branch = install + compat.ESIM_INSTALL_ADDRESS - compat.ESIM_INSTALL_GUARD_ADDRESS
        self.assertEqual(image[branch:branch + 4], struct.pack('<I', 0xd503201f))
        branch = eligibility + compat.ESIM_ELIGIBILITY_ADDRESS - compat.ESIM_ELIGIBILITY_GUARD_ADDRESS
        instruction = struct.unpack_from('<I', image, branch)[0]
        self.assertEqual(instruction & 0xff00001f, 0x34000008)  # Still CBZ W8: signed-material presence only.
        immediate = (instruction >> 5) & 0x7ffff
        if immediate & (1 << 18):
            immediate -= 1 << 19
        self.assertEqual(compat.ESIM_ELIGIBILITY_ADDRESS + immediate * 4, 0x1009aa9c0)
        expected = bytearray(compat.ESIM_ELIGIBILITY_GUARD)
        offset = compat.ESIM_ELIGIBILITY_ADDRESS - compat.ESIM_ELIGIBILITY_GUARD_ADDRESS
        expected[offset:offset + 4] = compat.ESIM_ELIGIBILITY
        self.assertEqual(image[eligibility:eligibility + len(expected)], expected)

    def test_firmware_follows_the_modem_files(self):
        sys.path.insert(0, str(ROOT / 'tools'))
        try:
            spec = importlib.util.spec_from_file_location('linux_log_baseband', ROOT / 'tools/linux_log.py')
            linux_log = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(linux_log)
        finally:
            sys.path.remove(str(ROOT / 'tools'))
        with tempfile.TemporaryDirectory(prefix='coachz-bb-blob-') as directory:
            directory = Path(directory)
            kpart = directory / 'kpart'
            kpart.write_bytes(bytes(4096))
            (directory / 'mba.mbn').write_bytes(b'm' * 5000)
            (directory / 'qdsp6sw.mbn').write_bytes(b'q' * 9000)
            plain, plain_end = linux_log.modem_blob(kpart, directory)
            pieces, end = linux_log.modem_blob(kpart, directory, b'CZBBFW01archive')
        self.assertEqual(struct.unpack_from('<I', plain[0][1], 16)[0], 2)
        header = pieces[0][1]
        self.assertEqual(struct.unpack_from('<I', header, 16)[0], 3)
        name, offset, size, digest = struct.unpack_from('<16sQQ32s', header, 32 + 64 * 2)
        self.assertEqual((name.rstrip(b'\0'), size, digest), (b'baseband.arc', 15, hashlib.sha256(b'CZBBFW01archive').digest()))
        self.assertEqual(pieces[-1], (linux_log.MODEM_BLOB + offset, b'CZBBFW01archive'))
        self.assertEqual((plain_end, end), (linux_log.MODEM_BLOB + offset, linux_log.MODEM_BLOB + offset + 4096))
        # The kernel's loop looks at eight entries, and the qdsp6sw.mbn loader comes before the archive.
        self.assertIn('i < count && i < 8', (PATCHES / 'coachz-prelink-baseband.patch').read_text())

    def test_firmware_archive(self):
        screen = load_screen()
        files = {'sbl1.mbn': b'\x7fELF' + bytes(300), 'Info.plist': b'<plist/>', 'Options.plist': b'', 'vinyl/update/main/info.plist': b'x' * 9}
        inner = io.BytesIO()
        with zipfile.ZipFile(inner, 'w', zipfile.ZIP_DEFLATED) as bbfw:
            bbfw.writestr('vinyl/', b'')
            for name, data in files.items():
                bbfw.writestr(name, data)
        with tempfile.TemporaryDirectory(prefix='coachz-bb-ipsw-') as directory:
            ipsw = Path(directory) / 'test.ipsw'
            with zipfile.ZipFile(ipsw, 'w') as outer:
                outer.writestr('Firmware/vinyl_02_Mav16-11.00.01.Release.bbfw', inner.getvalue())
                outer.writestr('Firmware/other.bbfw', b'')
            archive = screen.baseband_firmware(ipsw)
        self.assertEqual(archive[:8], b'CZBBFW01')
        count, _, total = struct.unpack_from('<IIQ', archive, 8)
        self.assertEqual((count, total), (len(files), len(archive)))
        at, seen = 24, {}
        for _ in range(count):
            name_bytes, mode, size, packed = struct.unpack_from('<IIQQ', archive, at)
            at += 24
            name = archive[at:at + name_bytes - 1].decode()
            self.assertEqual(archive[at + name_bytes - 1], 0)
            at += (name_bytes + 7) & ~7
            seen[name] = zlib.decompress(archive[at:at + packed])
            self.assertEqual((len(seen[name]), mode), (size, 0o644))
            self.assertGreater(packed, 0)
            at += (packed + 7) & ~7
        self.assertEqual(at, len(archive))
        self.assertEqual(seen, files)


if __name__ == '__main__':
    unittest.main()
