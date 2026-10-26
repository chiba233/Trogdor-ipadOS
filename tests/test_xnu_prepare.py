import importlib.util
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from fdt import Node, decode, encode, string, u32
from xnu_prepare import (WINDOW, boot_args, hardware_tree, live_tree, macho,
                         memory_map, select_window)

BASE = 0xfffffff007004000


def fixture():
    commands = []
    for name, va, mem, off, size, prot in ((b'__TEXT', BASE, 0x4000, 0x400, 32, 5),
                                          (b'__DATA', BASE + 0x4000, 0x4000, 0x420, 8, 3)):
        commands.append(struct.pack('<II16s4Q4I', 0x19, 72, name, va, mem, off, size, 7, prot, 0, 0))
    thread = bytearray(288)
    struct.pack_into('<4I', thread, 0, 5, 288, 6, 68)
    struct.pack_into('<Q', thread, 272, BASE)
    commands.append(thread)
    data = bytearray(struct.pack('<8I', 0xfeedfacf, 0x100000c, 0, 2, 3,
                                sum(map(len, commands)), 0, 0) + b''.join(commands))
    data.extend(bytes(0x428 - len(data)))
    data[0x400:0x420] = struct.pack('<I', 0xd503201f) * 8
    data[0x420:] = b'fixture!'
    return data


def tree():
    return decode(encode(Node('', {'#address-cells': u32(2), '#size-cells': u32(2),
                                 'compatible': string('qcom,sc7180')}, [
        Node('memory', {'device_type': string('memory'), 'reg': struct.pack('>4Q',
             0x80000000, 0x10000000, 0x100000000, 0x60000000)}),
        Node('reserved-memory', {'#address-cells': u32(2), '#size-cells': u32(2), 'ranges': b''}, [
            Node('secure', {'reg': struct.pack('>QQ', 0x100000000, 0x1000000)}),
            Node('dynamic', {'size': struct.pack('>Q', 0x4000),
                            'alloc-ranges': struct.pack('>QQ', 0x110000000, 0x10000000)})]),
        Node('cpus', {'#address-cells': u32(2), '#size-cells': u32(0)}, [
            Node('cpu@0', {'device_type': string('cpu'), 'reg': struct.pack('>Q', 0),
                          'compatible': string('arm,cortex-a55')})]),
        Node('soc', {'#address-cells': u32(2), '#size-cells': u32(2),
                     'ranges': struct.pack('>QQQ', 0, 0, 0x1000000000)}, [
            Node('intc', {'compatible': string('arm,gic-v3'),
                          'reg': struct.pack('>4Q', 0x17a00000, 0x10000, 0x17a60000, 0x100000)})])
    ]), [(0x101000000, 0x200000)]))


class XnuPrepareTests(unittest.TestCase):
    def test_macho_segment_and_entry_mapping(self):
        k = macho(fixture())
        self.assertEqual((k.base, k.end, k.entry), (BASE, BASE + 0x8000, BASE))
        self.assertEqual(k.segments[1][3], 8)

    def test_macho_rejects_wrong_abi_truncation_overflow_overlap_and_bss_entry(self):
        for n in (0, 31, 464, 0x400, 0x427):
            with self.subTest(length=n), self.assertRaises(ValueError):
                macho(fixture()[:n])
        changes = [(8, '<I', 2), (12, '<I', 0xc), (16, '<I', 99),
                   (32 + 4, '<I', 7), (32 + 24, '<Q', (1 << 64) - 4),
                   (32 + 40, '<Q', 1 << 63), (32 + 48, '<Q', 0x5000),
                   (104 + 24, '<Q', BASE), (176, '<I', 0x80000028),
                   (176 + 272, '<Q', BASE + 0x40), (32 + 48, '<Q', 3)]
        for offset, fmt, value in changes:
            data = fixture()
            struct.pack_into(fmt, data, offset, value)
            with self.subTest(offset=offset, value=value), self.assertRaises(ValueError):
                macho(data)

    def test_macho_refuses_unresolved_fixup_commands(self):
        for command in (0x80000034, 0x80000022, 0xc, 0xe, 0x80000018):
            data = fixture()
            struct.pack_into('<I', data, 176, command)
            with self.subTest(command=command), self.assertRaises(ValueError):
                macho(data)

    def test_rejects_highvm_and_protected_segment_placement(self):
        for flags in (1, 8, 0x80000000):
            data = fixture()
            struct.pack_into('<I', data, 32 + 68, flags)
            with self.subTest(flags=flags), self.assertRaises(ValueError):
                macho(data)
        for flags in (4, 0x10, 0x14):
            data = fixture()
            struct.pack_into('<I', data, 32 + 68, flags)
            self.assertEqual(macho(data).entry, BASE)

    def test_darwin23_boot_args_offsets_and_physical_virtual_translation(self):
        k = macho(fixture())
        raw, layout = boot_args(k, 0x120000000, WINDOW, 9001, '-v cpus=1')
        self.assertEqual(len(raw), 0x480)
        self.assertEqual(struct.unpack_from('<HH', raw), (2, 2))
        self.assertEqual(struct.unpack_from('<4Q', raw, 8),
                         (BASE, 0x120000000, WINDOW, layout['top_of_kernel_data']))
        self.assertEqual(raw[0x28:0x60], bytes(56))  # No unvalidated framebuffer.
        self.assertEqual(struct.unpack_from('<QI', raw, 0x60),
                         (BASE + layout['adt_pa'] - 0x120000000, 9001))
        self.assertEqual(raw[0x6c:0x6c + 10], b'-v cpus=1\0')
        self.assertEqual(struct.unpack_from('<Q', raw, 0x478)[0], WINDOW)
        self.assertEqual(layout['entry_pa'], 0x120000000)
        for line in ('x' * 1024, 'bad\0argument', '非 ASCII'):
            with self.subTest(line=line[:16]), self.assertRaises((ValueError, UnicodeError)):
                boot_args(k, 0x120000000, WINDOW, 1, line)
        with self.assertRaises(ValueError):
            boot_args(k, 0x120000001, WINDOW, 1, '')

    def test_boot_args_layout_compiles_against_pinned_apple_header(self):
        header = ROOT / 'upstream/apple-xnu/pexpert/pexpert/arm64/boot.h'
        zig = ROOT / '.tools/ziglang/ziglang/zig'
        if not header.is_file() or not zig.is_file():
            self.skipTest('optional Apple source or pinned cross compiler absent')
        with tempfile.TemporaryDirectory() as temp:
            temp = Path(temp)
            includes = temp / 'pexpert/arm'
            includes.mkdir(parents=True)
            for name in ('consistent_debug.h', 'protos.h'):
                (includes / name).write_text('/* unrelated declarations unused by boot_args */\n')
            code = '#include <stdint.h>\n#include <stddef.h>\n#include "' + str(header) + '"\n'
            code += '_Static_assert(sizeof(boot_args) == 0x480, "size");\n'
            for field, offset in [('virtBase', 8), ('Video', 0x28), ('deviceTreeP', 0x60),
                                  ('CommandLine', 0x6c), ('bootFlags', 0x470), ('memSizeActual', 0x478)]:
                code += f'_Static_assert(offsetof(boot_args, {field}) == {offset}, "{field}");\n'
            source = temp / 'abi.c'
            source.write_text(code)
            subprocess.run([str(zig), 'cc', '-target', 'aarch64-freestanding-none',
                '-I', str(temp), '-c', str(source), '-o', str(temp / 'abi.o')], check=True,
                capture_output=True)

    def test_exact_depthcharge_end_layout_only(self):
        original = encode(Node(''))
        data = bytearray(original)
        size = struct.unpack_from('>I', data, 36)[0]
        struct.pack_into('>I', data, 36, size - 4)
        self.assertEqual(live_tree(bytes(data)).encode(), original)
        for offset in (1, 8):
            bad = bytearray(data)
            struct.pack_into('>I', bad, 36, size - offset)
            with self.assertRaises(ValueError):
                live_tree(bytes(bad))
        with self.assertRaises(ValueError):
            live_tree(bytes(data[:-1]))

    def test_ram_banks_static_reservations_and_dynamic_ranges(self):
        ram, excluded = memory_map(tree())
        base, size = select_window(ram, excluded)
        self.assertEqual((base, size), (0x120000000, WINDOW))
        self.assertIn((0x110000000, 0x120000000), excluded)
        self.assertTrue(any(lo <= base and base + size <= hi for lo, hi in ram))
        self.assertFalse(any(lo < base + size and base < hi for lo, hi in excluded))

    def test_never_spans_holes_or_ignores_unbounded_reservations(self):
        with self.assertRaises(ValueError):
            select_window([(0x100000000, 0x108000000), (0x110000000, 0x118000000)], [])
        t = tree()
        del t.root.children[1].children[1].props['alloc-ranges']
        with self.assertRaises(ValueError):
            memory_map(t)
        t = tree()
        t.root.children[0].props['reg'] += struct.pack('>QQ', 0x100100000, 0x1000000)
        with self.assertRaises(ValueError):
            memory_map(t)

    def test_typed_hardware_tree_uses_measured_boot_cpu_timer_and_gic_mapping(self):
        t = tree()
        raw = hardware_tree(t, {'MPIDR': 0x81000000, 'CNTFRQ': 19200000}, 0x120000000, WINDOW)
        output = decode(raw).root
        self.assertEqual(output.children[1].props['timebase-frequency'], u32(19200000))
        self.assertEqual(len(output.children[1].children), 1)
        gic = output.children[2].children[0]
        self.assertEqual(gic.props['reg'], struct.pack('>4Q', 0x17a00000, 0x10000, 0x17a60000, 0x100000))
        with self.assertRaises(ValueError):
            hardware_tree(t, {'MPIDR': 0x81000100, 'CNTFRQ': 19200000}, 0x120000000, WINDOW)
        t.root.children[3].props['ranges'] = struct.pack('>QQQ', 0, 0x10000000, 0x1000000000)
        translated = decode(hardware_tree(t, {'MPIDR': 0, 'CNTFRQ': 1}, 0x120000000, WINDOW))
        self.assertEqual(struct.unpack_from('>Q', translated.root.children[2].children[0].props['reg'])[0], 0x27a00000)

    def test_hardware_tree_rejects_wrong_soc_and_zero_cpu_cells(self):
        for case in ('soc', 'cells'):
            t = tree()
            if case == 'soc':
                t.root.props['compatible'] = string('qcom,sm8550')
            else:
                t.root.children[2].props['#address-cells'] = u32(0)
            with self.subTest(case=case), self.assertRaises(ValueError):
                hardware_tree(t, {'MPIDR': 0, 'CNTFRQ': 19200000}, 0x120000000, WINDOW)

    def test_generated_adt_cross_checks_with_independent_upstream_decoder(self):
        base = ROOT / 'upstream/unixnu/scripts'
        if not base.is_dir():
            self.skipTest('optional pinned UniXNU checkout absent')
        sys.path.insert(0, str(base))
        from unixnu.devicetree import generate
        spec = importlib.util.spec_from_file_location('independent_adt', ROOT / 'upstream/ios-sweet/tools/adt.py')
        if not Path(spec.origin).is_file():
            self.skipTest('optional independent ADT decoder absent')
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / 'hardware.dtb'
            path.write_bytes(hardware_tree(tree(), {'MPIDR': 0, 'CNTFRQ': 19200000}, 0x120000000, WINDOW))
            blob = generate(path, {'adt': {'chosen': {'string': {'osenvironment': 'ramdisk'}}}})
        root, end = module.parse(blob)
        self.assertEqual(end, len(blob))
        nodes = {child.name: child for child in root.children}
        self.assertTrue({'chosen', 'cpus', 'arm-io', 'defaults', 'options'} <= nodes.keys())
        self.assertEqual(nodes['chosen'].prop('dram-base'), struct.pack('<Q', 0x120000000))
        self.assertEqual(len(nodes['chosen'].prop('random-seed')), 64)
        self.assertEqual(len(nodes['chosen'].prop('nvram-proxy-data')), 8192)
        self.assertEqual(nodes['arm-io'].children[0].prop('reg'),
                         struct.pack('<4Q', 0x17a00000 - 0x17000000, 0x10000,
                                     0x17a60000 - 0x17000000, 0x100000))
