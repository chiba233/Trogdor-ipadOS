"""Pending switches checks. No kernel objects, packing or device access."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
PENDING = ROOT / 'platform/coachz-prelink/pending/switches'


class SwitchesTests(unittest.TestCase):
    def test_query_and_transition_model(self):
        compiler = shutil.which('cc')
        self.assertIsNotNone(compiler)
        with tempfile.TemporaryDirectory(prefix='coachz-switches-host-') as directory:
            binary = Path(directory) / 'switches-model'
            subprocess.run([compiler, '-std=c11', '-Wall', '-Wextra', '-Werror', '-I', str(PENDING),
                            str(ROOT / 'tests/coachz_switches_model_test.c'), '-o', str(binary)], check=True)
            output = subprocess.run([str(binary)], check=True, capture_output=True, text=True).stdout
            self.assertEqual(output.count('PASS '), 6, output)

    def test_portable_header_cpp_syntax(self):
        compiler = shutil.which('c++')
        self.assertIsNotNone(compiler)
        subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-fsyntax-only',
                        '-x', 'c++', '-I', str(PENDING), '-'],
                       input='#include "coachz_switches.h"\nint main() { cs_tracker t{}; return t.seen; }\n',
                       text=True, check=True)

    def test_candidate_patch_matches_sources(self):
        patch = shutil.which('patch')
        self.assertIsNotNone(patch)
        with tempfile.TemporaryDirectory(prefix='coachz-switches-patch-') as directory:
            tree = Path(directory)
            (tree / 'iokit/conf').mkdir(parents=True)
            (tree / 'iokit/Kernel').mkdir()
            (tree / 'bsd/dev').mkdir(parents=True)
            # Padding allows BSD patch to search for the contexts at different line numbers.
            (tree / 'iokit/conf/files').write_text('\n' * 200 +
                'iokit/Kernel/CoachzBattery.cpp\t\t\toptional iokitcpp\n'
                'iokit/Kernel/CoachzSpeaker.cpp\t\t\toptional iokitcpp\n')
            (tree / 'bsd/dev/coachz_sample.c').write_text('\n' * 200 +
                '\tcoachz_input_start();\n'
                '\t/* The Chrome EC battery service (iokit/Kernel/CoachzBattery.cpp, coachz-battery=1): IOKit is up by the time this thread runs. */\n')
            result = subprocess.run([patch, '--batch', '--fuzz=0', '-p1', '-i',
                                     str(PENDING / 'coachz-prelink-switches.patch')],
                                    cwd=tree, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            for name in ['CoachzSwitches.cpp', 'coachz_switches.h']:
                self.assertEqual((tree / 'iokit/Kernel' / name).read_bytes(), (PENDING / name).read_bytes())
            self.assertIn('coachz_switches_start();', (tree / 'bsd/dev/coachz_sample.c').read_text())


if __name__ == '__main__':
    unittest.main()
