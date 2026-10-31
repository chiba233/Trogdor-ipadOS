"""Pending TSENS fixtures only. Never builds or boots a kernel."""
import importlib.util
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
PENDING = ROOT / 'platform/coachz-prelink/pending/thermal'


class ThermalTests(unittest.TestCase):
    def test_sampling_fixtures(self):
        compiler = shutil.which('cc')
        self.assertIsNotNone(compiler)
        with tempfile.TemporaryDirectory(prefix='coachz-thermal-host-') as directory:
            binary = Path(directory) / 'thermal-model'
            subprocess.run([compiler, '-std=c11', '-Wall', '-Wextra', '-Werror', '-I', str(PENDING),
                            str(ROOT / 'tests/coachz_thermal_model_test.c'), '-o', str(binary)], check=True)
            output = subprocess.run([str(binary)], check=True, capture_output=True, text=True).stdout
            self.assertEqual(output.count('PASS '), 6, output)

    def test_adt_helper_with_actual_loader_node(self):
        sys.path.insert(0, str(ROOT / 'tools'))
        try:
            import xnu_screen
            spec = importlib.util.spec_from_file_location('pending_thermal_adt', PENDING / 'adt.py')
            helper = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(helper)
            root = xnu_screen.Node()
            root.set('name', b'device-tree\0')
            helper.add_thermal_sampler(root)
            node = xnu_screen.read_tree(root.write()).child('arm-io').child('coachz-thermal')
            props = {key: value for key, _, value in node.props}
            self.assertEqual(props['compatible'], b'google,coachz-tsens-v2\0')
            self.assertEqual(struct.unpack('<8Q', props['reg']),
                             (0x0c263000, 0x200, 0x0c222000, 0x200,
                              0x0c265000, 0x200, 0x0c223000, 0x200))
        finally:
            sys.path.pop(0)

    def test_pending_patch_applies_and_matches_sources(self):
        patch = shutil.which('patch')
        self.assertIsNotNone(patch)
        with tempfile.TemporaryDirectory(prefix='coachz-thermal-patch-') as directory:
            tree = Path(directory)
            (tree / 'bsd/conf').mkdir(parents=True)
            (tree / 'bsd/dev').mkdir()
            (tree / 'bsd/conf/files').write_text(
                '\n' * 109 + 'bsd/dev/coachz_touch.c\t\tstandard\n'
                'bsd/dev/coachz_ec.c\t\tstandard\n'
                'bsd/dev/mem.c\t\t\t\tstandard\n')
            # BSD patch needs enough lines to locate both sampler hunks.
            (tree / 'bsd/dev/coachz_sample.c').write_text(
                '\n' * 40 + '\tunsigned ticks = 0;\n'
                '\textern void coachz_battery_start(void);\n' + '\n' * 16 +
                '\t\t(void)tsleep(&chan, PZERO, "coachz", 10 * hz);\n'
                '\t\t/* Keep the captured note readable on the wrapping screen console. */\n')
            applied = subprocess.run([patch, '--batch', '--fuzz=0', '-p1', '-i',
                                      str(PENDING / 'coachz-prelink-thermal.patch')],
                                     cwd=tree, capture_output=True, text=True)
            self.assertEqual(applied.returncode, 0, applied.stdout + applied.stderr)
            for name in ['coachz_thermal.c', 'coachz_tsens.h']:
                self.assertEqual((tree / 'bsd/dev' / name).read_bytes(), (PENDING / name).read_bytes(), name)
            sampler = (tree / 'bsd/dev/coachz_sample.c').read_text()
            self.assertIn('coachz_thermal_start();', sampler)
            self.assertIn('coachz_thermal_poll();', sampler)


if __name__ == '__main__':
    unittest.main()
