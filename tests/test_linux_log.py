import hashlib
import lzma
from pathlib import Path
import struct
import sys
import tempfile
import unittest
import zlib

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from artifacts import inspect_image
from disk import fragments
from fdt import Node, decode, encode, properties, string
from linux_log import LOG_PROPERTY, add_log, compile_wrapper, hashes, linux_image
from read_probe import read_record


class LinuxLogTests(unittest.TestCase):
    def image(self):
        return struct.pack('<II6QII', 0x91005a4d, 0x14000001, 0, 4096,
                           0xa, 0, 0, 0, 0x644d5241, 0) + bytes(64)

    def test_full_tree_roundtrip_preserves_empty_nodes_reservations_and_header(self):
        original = encode(Node('', {'compatible': string('test')}, [Node('empty')]),
                          [(0x80000000, 0x1000)], 7, 2)
        tree = decode(original)
        self.assertEqual(tree.root.children[0].name, 'empty')
        self.assertEqual(tree.reservations, ((0x80000000, 0x1000),))
        self.assertEqual(tree.boot_cpuid_phys, 7)
        self.assertEqual(tree.last_comp_version, 2)
        self.assertEqual(tree.encode(), original)

    def test_decoder_rejects_truncation_and_unterminated_or_overlapping_regions(self):
        original = encode(Node(''))
        for n in range(len(original)):
            with self.subTest(length=n), self.assertRaises(ValueError):
                decode(original[:n])
        for offset, value in ((16, 0), (8, 40), (12, 40), (24, 18), (36, 0xffffffff)):
            bad = bytearray(original)
            struct.pack_into('>I', bad, offset, value)
            with self.subTest(offset=offset), self.assertRaises(ValueError):
                decode(bad)
        bad = bytearray(original)
        struct.pack_into('>QQ', bad, 40, 1, 1)
        with self.assertRaises(ValueError):
            decode(bad)

    def test_only_chosen_log_property_is_added_to_full_dtb(self):
        old = encode(Node('', {'x': bytes(range(20))}, [Node('empty'),
                     Node('chosen', {'bootargs': string('existing arguments')})]),
                     [(0x90000000, 0x2000)], 3)
        new = add_log(old)
        expected = properties(old)
        expected[('/chosen', LOG_PROPERTY)] = bytes(8192)
        self.assertEqual(properties(new), expected)
        self.assertEqual(decode(new).reservations, decode(old).reservations)
        self.assertEqual(decode(new).boot_cpuid_phys, 3)
        self.assertEqual(decode(new).root.children[0].name, 'empty')
        with self.assertRaises(ValueError):
            add_log(new)
        self.assertIn(('/chosen', LOG_PROPERTY), properties(add_log(encode(Node('')))))

    def test_linux_abi_and_compression_guards(self):
        image = self.image()
        self.assertEqual(linux_image(image, 'none'), (image, 4096))
        self.assertEqual(linux_image(lzma.compress(image), 'lzma'), (image, 4096))
        for offset, value in ((8, 0x80000), (16, 64), (24, 0xb), (24, 2), (56, 0)):
            bad = bytearray(image)
            struct.pack_into('<Q' if offset != 56 else '<I', bad, offset, value)
            with self.subTest(offset=offset, value=value), self.assertRaises(ValueError):
                linux_image(bad, 'none')
        with self.assertRaises(ValueError):
            linux_image(image, 'gzip')

    def test_fit_hashes_detect_tampering_and_are_recomputed(self):
        for algo, digest in [('crc32', struct.pack('>I', zlib.crc32(b'payload'))),
                             ('sha256', hashlib.sha256(b'payload').digest())]:
            node = Node('kernel', {'data': b'payload'}, [Node('hash', {
                'algo': string(algo), 'value': digest})])
            hashes(node)
            node.props['data'] = b'changed'
            with self.assertRaises(ValueError):
                hashes(node)
            hashes(node, True)
            hashes(node)

    def record(self, current=1):
        text = ('Trogdor-ipadOS native phase-0 probe\n' +
                'entry EL=0x0000000000000002\n' +
                f'current EL=0x{current:016x}\n' +
                'stage=0x0000000000000003\n' +
                'handoff EL=0x0000000000000002\n').encode()
        return text + bytes(8192 - len(text))

    def test_reader_requires_direct_complete_el1_record(self):
        text, fields = read_record(self.record())
        self.assertEqual(fields['current EL'], 1)
        self.assertIn('handoff EL=', text)
        for bad in (bytes(8192), self.record(2), self.record()[:-1], b'x' * 8192):
            with self.assertRaises(ValueError):
                read_record(bad)

    def test_large_gpt_layout_supports_payload_beyond_old_16mib_limit(self):
        payload = b'CHROMEOS' + bytes(17 * 1024 * 1024 - 8)
        with self.assertRaises(ValueError):
            fragments(payload)
        parts = fragments(payload, disk_mib=128, kernel_mib=96)
        self.assertEqual(parts[-1][0], 128 * 1024 * 1024 - 512)
        self.assertEqual(struct.unpack_from('<Q', parts[1][1], 32)[0], 262143)
        self.assertEqual(struct.unpack_from('<Q', parts[2][1], 40)[0], 2048 + 96 * 2048 - 1)
        self.assertEqual(parts[3][1], payload)
        for disk, kernel in ((64, 64), (128, 0), (32769, 96), (True, 1)):
            with self.subTest(disk=disk, kernel=kernel), self.assertRaises(ValueError):
                fragments(payload, disk_mib=disk, kernel_mib=kernel)

    def test_compiled_wrapper_keeps_payload_aligned_and_reserves_linux_memory(self):
        with tempfile.TemporaryDirectory() as temp:
            image, report = compile_wrapper(Path(temp), self.image(), 4096)
            self.assertEqual(report['linux_offset'], 0x200000)
            self.assertEqual(image[0x200000:], self.image())
            self.assertEqual(inspect_image(image)['memory_size'], 0x201000)
            self.assertEqual(image.count(struct.pack('<I', 0xd4000002 | (0x5452 << 5))), 1)
