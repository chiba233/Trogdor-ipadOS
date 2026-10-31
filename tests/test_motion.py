"""Pending EC motion checks. No XNU object compilation or device access."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
PENDING = ROOT / 'platform/coachz-prelink/pending/motion'


class MotionTests(unittest.TestCase):
    def test_command_and_sampling_model(self):
        compiler = shutil.which('cc')
        self.assertIsNotNone(compiler)
        with tempfile.TemporaryDirectory(prefix='coachz-motion-host-') as directory:
            binary = Path(directory) / 'motion-model'
            subprocess.run([compiler, '-std=c11', '-Wall', '-Wextra', '-Werror', '-I', str(PENDING),
                            str(ROOT / 'tests/coachz_motion_model_test.c'), '-o', str(binary)], check=True)
            output = subprocess.run([str(binary)], check=True, capture_output=True, text=True).stdout
            self.assertEqual(output.count('PASS '), 6, output)

    def test_portable_header_is_usable_from_cpp(self):
        compiler = shutil.which('c++')
        self.assertIsNotNone(compiler)
        subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-fsyntax-only',
                        '-x', 'c++', '-I', str(PENDING), '-'],
                       input='#include "coachz_motion.h"\nint main() { cm_sample s{}; return s.error; }\n',
                       text=True, check=True)

    def test_candidate_patch_applies_and_matches_sources(self):
        patch = shutil.which('patch')
        self.assertIsNotNone(patch)
        with tempfile.TemporaryDirectory(prefix='coachz-motion-patch-') as directory:
            tree = Path(directory)
            (tree / 'iokit/conf').mkdir(parents=True)
            (tree / 'iokit/Kernel').mkdir()
            (tree / 'bsd/dev').mkdir(parents=True)
            (tree / 'iokit/conf/files').write_text(
                '\n' * 51 + 'iokit/Kernel/IOPMPowerSource.cpp\t\t\toptional iokitcpp\n'
                'iokit/Kernel/CoachzInput.cpp\t\t\toptional iokitcpp\n')
            (tree / 'bsd/dev/coachz_sample.c').write_text(
                '\n' * 47 + '\tcoachz_battery_start();\n'
                '\t/* A one-second tone on the speaker (iokit/Kernel/CoachzSpeaker.cpp, coachz-speaker=1), once. */\n')
            applied = subprocess.run([patch, '--batch', '--fuzz=0', '-p1', '-i',
                                      str(PENDING / 'coachz-prelink-motion.patch')],
                                     cwd=tree, capture_output=True, text=True)
            self.assertEqual(applied.returncode, 0, applied.stdout + applied.stderr)
            for name in ['CoachzMotion.cpp', 'coachz_motion.h']:
                self.assertEqual((tree / 'iokit/Kernel' / name).read_bytes(), (PENDING / name).read_bytes(), name)
            self.assertIn('coachz_motion_start();', (tree / 'bsd/dev/coachz_sample.c').read_text())


if __name__ == '__main__':
    unittest.main()
