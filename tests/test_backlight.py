"""Host checks of the panel backlight (platform/coachz-prelink/package/CoachzBacklight.cpp)."""
import importlib.util
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
PACKAGE = ROOT / 'platform/coachz-prelink/package'
SOURCE = (PACKAGE / 'CoachzBacklight.cpp').read_text()
REFERENCE = ROOT / 'build/coachz-display-reference/kernel.macho'


class MappingTests(unittest.TestCase):
    def test_duty_luminance_and_fade(self):
        start = SOURCE.index('// Duty and nominal luminance')
        functions = SOURCE[start:SOURCE.index('// End of the host-tested functions.')]
        with tempfile.TemporaryDirectory() as directory:
            test = Path(directory) / 'mapping.cpp'
            test.write_text('#include <stdint.h>\n#include <stdio.h>\n' + functions + r'''
int main() {
    uint32_t last = 0;
    for (uint32_t m = 0; m <= 450000; m += 50) {
        uint32_t d = dutyForMilliNits(m);
        if (d < kMinDuty || d > kMaxDuty || d < last) return 1;       // lit, in range, monotonic
        last = d;
        if (m >= kMinMilliNits && m <= kMaxMilliNits) {
            int32_t back = (int32_t)milliNitsForDuty(d) - (int32_t)m;
            if (back < -100 || back > 100) return 2;                    // the inverse within one duty step
        }
    }
    for (uint32_t l = 0; l <= 255; l++)
        if (levelForMilliNits(milliNitsForLevel(l)) != l) return 3;
    if (dutyForMilliNits(0) != 3136 || dutyForMilliNits(400000) != 65535 || dutyForMilliNits(milliNitsForLevel(0)) != 3136) return 4;
    if (milliNitsForFixed(-(5 << 16)) != 0 || milliNitsForFixed(100 << 16) != 100000 || milliNitsForFixed(900 << 16) != 400000) return 5;
    if (fadeMsForFixed(1 << 15) != 500 || fadeMsForFixed(60u << 16) != 5000) return 6;
    if (fadeDuty(1000, 3000, 0, 400) != 1000 || fadeDuty(1000, 3000, 200, 400) != 2000 || fadeDuty(3000, 1000, 100, 400) != 2500 ||
        fadeDuty(1000, 3000, 400, 400) != 3000 || fadeDuty(1000, 3000, 7, 0) != 3000) return 7;
    printf("PASS\n");
    return 0;
}
''')
            binary = Path(directory) / 'mapping'
            subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function', '-Wno-unused-const-variable', str(test), '-o',
                            str(binary)], check=True)
            self.assertIn('PASS', subprocess.run([str(binary)], check=True, capture_output=True, text=True).stdout)


class SourceTests(unittest.TestCase):
    def test_only_display_pwm_reaches_the_ec(self):
        self.assertEqual(set(re.findall(r'(?:return|=) coachz_ec_command\((\w+)', SOURCE)), {'kPwmSetDuty', 'kPwmGetDuty'})
        self.assertIn('kPwmSetDuty = 0x0025, kPwmGetDuty = 0x0026', SOURCE)
        self.assertIn('{(uint8_t)duty, (uint8_t)(duty >> 8), kPwmTypeDisplayLight, 0}', SOURCE)   # typed DISPLAY_LIGHT, index 0
        self.assertIn('kPwmTypeDisplayLight = 2', SOURCE)
        self.assertNotIn('GPIO', SOURCE.split('#include', 1)[1].split('class CoachzBacklight', 1)[1])

    def test_property_requests_never_wait_for_the_ec(self):
        body = SOURCE[SOURCE.index('IOReturn setProperties('):SOURCE.index('void free() override')]
        self.assertNotIn('pwm', body.lower().replace('kpwm', ''))
        self.assertIn('post(dutyForMilliNits(milliNits)', body)

    def test_every_original_override_is_replaced(self):
        cls = SOURCE[SOURCE.index('class CoachzBacklight'):SOURCE.index('OSDefineMetaClassAndStructors')]
        for method in ['init(', 'start(', 'setProperties(', 'free(', 'getWorkLoop(', 'configureReport(', 'updateReport(',
                       'callPlatformFunction(', 'message(', 'setPowerState(']:
            self.assertRegex(cls, re.escape(method) + r'[^)]*\)( const)? override', method)
        self.assertNotIn('AppleARMBacklight::', cls)

    def test_display_starts_the_service(self):
        self.assertIn('coachz_backlight_start(display)', (PACKAGE / 'CoachzDisplay.cpp').read_text())


@unittest.skipUnless(REFERENCE.is_file(), 'needs the local prelinked ABI reference (ignored build output)')
class AbiTests(unittest.TestCase):
    def test_original_class_qualifies(self):
        sys.path.insert(0, str(ROOT / 'upstream/unixnu/kits/XnuPrelink'))
        spec = importlib.util.spec_from_file_location('build_coachz_backlight', ROOT / 'platform/coachz-prelink/scripts/build-coachz-backlight.py')
        builder = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(builder)
        _, methods, bindings = builder.qualify(REFERENCE)
        self.assertEqual(len(methods), 169)
        self.assertEqual(bindings['__ZTV17AppleARMBacklight'], 0xfffffff005ccbd18 - 16)


if __name__ == '__main__':
    unittest.main()
