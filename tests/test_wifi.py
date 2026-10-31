"""Portable fixtures for pending Wi-Fi code. Never builds or boots a kernel."""
import importlib.util
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
PENDING = ROOT / 'platform/coachz-prelink/pending/wifi'


class WifiTests(unittest.TestCase):
    def test_wire_and_smem_fixtures(self):
        compiler = shutil.which('cc')
        self.assertIsNotNone(compiler, 'a host C compiler is required for the protocol fixtures')
        with tempfile.TemporaryDirectory(prefix='coachz-wifi-host-') as directory:
            binary = Path(directory) / 'wifi-model'
            subprocess.run([compiler, '-std=c11', '-Wall', '-Wextra', '-Werror', '-I', str(PENDING),
                            str(ROOT / 'tests/coachz_wifi_model_test.c'), '-o', str(binary)], check=True)
            output = subprocess.run([str(binary)], check=True, capture_output=True, text=True).stdout
            self.assertEqual(output.count('PASS '), 7, output)

    def test_adt_helper_with_real_loader_node(self):
        sys.path.insert(0, str(ROOT / 'tools'))
        try:
            import xnu_screen
            spec = importlib.util.spec_from_file_location('pending_wifi_adt', PENDING / 'adt.py')
            helper = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(helper)
            root = xnu_screen.Node()
            root.set('name', b'device-tree\0')
            helper.add_wifi_inventory(root, 0x80900000, 0x200000)
            node = xnu_screen.read_tree(root.write()).child('arm-io').child('coachz-wifi')
            properties = {key: value for key, _, value in node.props}
            self.assertEqual(properties['compatible'], b'google,coachz-wcn3990\0')
            self.assertEqual(properties['smem-region'], bytes.fromhex('00009080000000000000200000000000'))
            with self.assertRaises(ValueError):
                helper.add_wifi_inventory(root, 0x86000000, 0x200000)
        finally:
            sys.path.pop(0)

    def test_candidate_patch_applies_and_matches_sources(self):
        patch = shutil.which('patch')
        self.assertIsNotNone(patch)
        with tempfile.TemporaryDirectory(prefix='coachz-wifi-patch-') as directory:
            tree = Path(directory)
            (tree / 'bsd/conf').mkdir(parents=True)
            (tree / 'bsd/dev').mkdir()
            (tree / 'bsd/conf/files').write_text(
                'bsd/dev/coachz_touch.c\t\tstandard\n'
                'bsd/dev/coachz_ec.c\t\tstandard\n'
                'bsd/dev/mem.c\t\t\t\tstandard\n')
            (tree / 'bsd/dev/coachz_sample.c').write_text(
                '\tstatic int chan;\n\tunsigned ticks = 0;\n'
                '\textern void coachz_battery_start(void);\n')
            applied = subprocess.run([patch, '--batch', '--fuzz=0', '-p1', '-i', str(PENDING / 'coachz-prelink-wifi.patch')],
                                     cwd=tree, capture_output=True, text=True)
            self.assertEqual(applied.returncode, 0, applied.stdout + applied.stderr)
            for name in ['coachz_wifi.c', 'coachz_wifi_proto.h', 'coachz_wifi_smem.h']:
                self.assertEqual((tree / 'bsd/dev' / name).read_bytes(), (PENDING / name).read_bytes(), name)
            self.assertIn('coachz_wifi_start();', (tree / 'bsd/dev/coachz_sample.c').read_text())


if __name__ == '__main__':
    unittest.main()
