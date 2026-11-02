"""Host checks for the pending IPA QMI handshake (modem data path). Never builds or boots a kernel."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
PENDING = ROOT / 'platform/coachz-prelink/pending/modem'


class IpaQmiTests(unittest.TestCase):
    def test_portable_model(self):
        cc = shutil.which('cc')
        self.assertIsNotNone(cc)
        with tempfile.TemporaryDirectory(prefix='coachz-ipa-qmi-') as directory:
            binary = Path(directory) / 'ipa-qmi-model'
            subprocess.run([cc, '-std=c11', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined', '-I', str(PENDING),
                            str(ROOT / 'tests/coachz_ipa_qmi_model_test.c'), '-o', str(binary)], check=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual(result.stdout.count('PASS '), 3, result.stdout)

    def test_probe_config_model(self):
        cc = shutil.which('cc')
        with tempfile.TemporaryDirectory(prefix='coachz-ipa-config-') as directory:
            binary = Path(directory) / 'ipa-config-model'
            subprocess.run([cc, '-std=c11', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined', '-I', str(PENDING),
                            str(ROOT / 'tests/coachz_ipa_config_model_test.c'), '-o', str(binary)], check=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual(result.stdout.count('PASS '), 4, result.stdout)

    def test_gsi_model(self):
        with tempfile.TemporaryDirectory(prefix='coachz-gsi-') as directory:
            binary = Path(directory) / 'gsi-model'
            subprocess.run([shutil.which('cc'), '-std=c11', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined', '-I', str(PENDING),
                            str(ROOT / 'tests/coachz_gsi_model_test.c'), '-o', str(binary)], check=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual(result.stdout.count('PASS '), 4, result.stdout)

    def test_command_model(self):
        with tempfile.TemporaryDirectory(prefix='coachz-ipa-cmd-') as directory:
            binary = Path(directory) / 'ipa-cmd-model'
            subprocess.run([shutil.which('cc'), '-std=c11', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined', '-I', str(PENDING),
                            str(ROOT / 'tests/coachz_ipa_cmd_model_test.c'), '-o', str(binary)], check=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual(result.stdout.count('PASS '), 4, result.stdout)

    def test_rx_model(self):
        with tempfile.TemporaryDirectory(prefix='coachz-ipa-rx-') as directory:
            binary = Path(directory) / 'ipa-rx-model'
            subprocess.run([shutil.which('cc'), '-std=c11', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined', '-I', str(PENDING),
                            str(ROOT / 'tests/coachz_ipa_rx_model_test.c'), '-o', str(binary)], check=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual(result.stdout.count('PASS '), 3, result.stdout)

    def test_modem_model(self):
        with tempfile.TemporaryDirectory(prefix='coachz-ipa-modem-') as directory:
            binary = Path(directory) / 'ipa-modem-model'
            subprocess.run([shutil.which('cc'), '-std=c11', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined', '-I', str(PENDING),
                            str(ROOT / 'tests/coachz_ipa_modem_model_test.c'), '-o', str(binary)], check=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual(result.stdout.count('PASS '), 3, result.stdout)

    def test_kernel_file_type_checks_against_stubs(self):
        from test_mmc import STUBS
        with tempfile.TemporaryDirectory(prefix='coachz-ipa-stubs-') as directory:
            stubs = Path(directory)
            for name, text in {**STUBS, 'kern/clock.h': STUBS['kern/clock.h'] + 'void delay(int);\n',
                                 'sys/errno.h': STUBS['sys/errno.h'] + '#define ENXIO 6\n#define E2BIG 7\n#define EBUSY 16\n'}.items():
                (stubs / name).parent.mkdir(parents=True, exist_ok=True)
                (stubs / name).write_text(text)
            for source in ('coachz_ipa_config.c', 'coachz_gsi.c', 'coachz_ipa_cmd.c', 'coachz_ipa_rx.c', 'coachz_ipa_modem.c'):
                result = subprocess.run([shutil.which('cc'), '-std=gnu11', '-fsyntax-only', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
                                         '-I', str(stubs), '-I', str(PENDING), str(PENDING / source)], capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, source + result.stderr)
            # drivers/RamBlock/coachz_modem.c includes these three in one translation unit: their static names must not collide
            unit = stubs / 'one_unit.c'
            unit.write_text(''.join('#include "%s"\n' % name for name in ('coachz_ipa_cmd.c', 'coachz_ipa_rx.c', 'coachz_ipa_modem.c')))
            result = subprocess.run([shutil.which('cc'), '-std=gnu11', '-fsyntax-only', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
                                     '-I', str(stubs), '-I', str(PENDING), str(unit)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, 'one unit' + result.stderr)

    def test_generated_data_names_its_source(self):
        text = (PENDING / 'coachz_ipa_v42.h').read_text()
        self.assertIn('6628e7d4c6b25c8d18f1345e3a20d21829aae7e4', text)
        self.assertIn('#define CIPA_SHARED_MEM_SIZE 0x00054', text)
        self.assertNotIn('not on this version */\n#define', text)

    def test_not_in_the_active_build(self):
        series = (ROOT / 'platform/coachz-prelink/patches/xnu/series').read_text()
        self.assertNotIn('ipa-qmi', series)
        self.assertNotIn('coachz_ipa_qmi', (ROOT / 'tools/xnu_screen.py').read_text())


if __name__ == '__main__':
    unittest.main()
