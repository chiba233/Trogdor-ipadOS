"""Host checks for the pending usb_1 SoC bring-up (DWC3, QUSB2, QMP). Never builds or boots a kernel."""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

from test_mmc import STUBS as MMC_STUBS

ROOT = Path(__file__).resolve().parents[1]
PENDING = ROOT / 'platform/coachz-prelink/pending/usb'
STUBS = dict(MMC_STUBS)
STUBS['sys/systm.h'] = 'int printf(const char *, ...);\n__attribute__((noreturn)) void panic(const char *, ...);\n'


class UsbSocTests(unittest.TestCase):
    def compiler(self):
        cc = shutil.which('cc')
        self.assertIsNotNone(cc, 'a host C compiler is required for the fixtures')
        return cc

    def test_portable_model(self):
        with tempfile.TemporaryDirectory(prefix='coachz-usb-host-') as directory:
            binary = Path(directory) / 'usb-model'
            subprocess.run([self.compiler(), '-std=c11', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                            '-I', str(PENDING), str(ROOT / 'tests/coachz_usb_model_test.c'), '-o', str(binary)], check=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual(result.stdout.count('PASS '), 7, result.stdout)

    def test_kernel_file_type_checks_against_stubs(self):
        with tempfile.TemporaryDirectory(prefix='coachz-usb-stubs-') as directory:
            stubs = Path(directory)
            for name, text in STUBS.items():
                (stubs / name).parent.mkdir(parents=True, exist_ok=True)
                (stubs / name).write_text(text)
            result = subprocess.run([self.compiler(), '-std=gnu11', '-fsyntax-only', '-Wall', '-Wextra', '-Werror',
                                     '-Wno-unused-function', '-I', str(stubs), '-I', str(PENDING), str(PENDING / 'coachz_usb_soc.c')],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_generated_tables_name_their_source(self):
        text = (PENDING / 'coachz_qmp_tables.h').read_text()
        self.assertIn('6628e7d4c6b25c8d18f1345e3a20d21829aae7e4', text)
        names = re.findall(r'static const struct cu_qmp_reg (cu_\w+)\[\]', text)
        self.assertEqual(names, ['cu_v3_usb3_serdes_tbl', 'cu_v3_usb3_tx_tbl', 'cu_v3_usb3_rx_tbl', 'cu_v3_usb3_pcs_tbl'])
        entries = re.findall(r'\{ 0x([0-9a-f]{4}), 0x([0-9a-f]{2}), 0x([0-9a-f]{2}) \}, /\* (Q\w+) \*/', text)
        self.assertEqual(len(entries), text.count('}, /*'))
        self.assertTrue(all(name.startswith(('QSERDES_V3_', 'QPHY_V3_PCS_')) for *_, name in entries))

    def test_not_in_the_active_build(self):
        series = (ROOT / 'platform/coachz-prelink/patches/xnu/series').read_text()
        self.assertNotIn('usb-soc', series)
        for path in [ROOT / 'tools/xnu_screen.py', ROOT / 'platform/coachz-prelink/drivers/RamBlock/build-usb-stack.py',
                     ROOT / 'platform/coachz-prelink/drivers/RamBlock/coachz_usb.c']:
            text = path.read_text()
            self.assertNotIn('coachz-usb-soc', text, path)
            self.assertNotIn('coachz_usb_soc', text, path)


if __name__ == '__main__':
    unittest.main()
