"""Host checks of the not-yet-enabled battery and speaker patches: the code under test is taken from the patch files themselves."""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
PATCHES = ROOT / 'platform/coachz-prelink/patches/xnu'


def new_file(patch, path):
    """The full text of a file a patch adds (its single hunk of '+' lines)."""
    text = (PATCHES / patch).read_text()
    start = text.index('+++ b/' + path + '\n')
    hunk = text[start:].split('\n', 2)[2]
    lines = []
    for line in hunk.split('\n'):
        if line.startswith('--- ') or not line.startswith('+'):
            break
        lines.append(line[1:])
    return '\n'.join(lines) + '\n'


class BatteryTests(unittest.TestCase):
    def test_protocol_framing_and_decoding(self):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            (work / 'coachz_ec_proto.h').write_text(new_file('coachz-prelink-battery.patch', 'bsd/dev/coachz_ec_proto.h'))
            binary = work / 'proto-test'
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-I', str(work),
                            str(ROOT / 'tests/coachz_ec_proto_test.c'), '-o', str(binary)], check=True)
            self.assertIn('PASS', subprocess.run([str(binary)], check=True, capture_output=True, text=True).stdout)

    def test_only_queries_reach_the_ec(self):
        # The service reads; it never sends charger, PD or EC configuration commands.
        battery = new_file('coachz-prelink-battery.patch', 'iokit/Kernel/CoachzBattery.cpp')
        commands = set(re.findall(r'coachz_ec_command\((EC_CMD_\w+)', battery))
        self.assertEqual(commands, {'EC_CMD_BATTERY_GET_DYNAMIC', 'EC_CMD_CHARGE_STATE'})
        self.assertIn('uint8_t reply[EC_MAX_REPLY], index = 0, charge[10] = {0};', battery)   # GET_STATE is subcommand 0
        self.assertIn('if (!published) {', battery)
        transport = new_file('coachz-prelink-battery.patch', 'bsd/dev/coachz_ec.c')
        self.assertIn('(void)chip_select(OP_CS_DEASSERT);', transport)
        self.assertIn('lck_mtx_lock(ec_lock);', transport)


class SpeakerTests(unittest.TestCase):
    def test_tone_is_440_hz_at_minus_18_dbfs(self):
        source = new_file('coachz-prelink-speaker.patch', 'iokit/Kernel/CoachzSpeaker.cpp')
        function = source[source.index('void fillTone('):source.index('}  // namespace')]
        with tempfile.TemporaryDirectory() as directory:
            test = Path(directory) / 'tone.c'
            test.write_text('#include <stdint.h>\n#include <stdio.h>\n' + function + r'''
int main(void) {
    static uint32_t w[48000];
    fillTone(w, 48000);
    int high = 0, low = 0, crossings = 0, tail = 0;
    int16_t previous = 0;
    for (int i = 0; i < 48000; i++) {
        int16_t left = (int16_t)(w[i] & 0xffff), right = (int16_t)(w[i] >> 16);
        if (left != right) return 1;
        if (left > high) high = left;
        if (left < low) low = left;
        if (i && ((previous < 0) != (left < 0))) crossings++;
        if (i >= 47000 && left > tail) tail = left;
        previous = left;
    }
    printf("%d %d %d %d\n", high, low, crossings, tail);
    return 0;
}
''')
            binary = Path(directory) / 'tone'
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', str(test), '-o', str(binary)], check=True)
            high, low, crossings, tail = map(int, subprocess.run([str(binary)], check=True, capture_output=True,
                                                                  text=True).stdout.split())
        self.assertTrue(4050 <= high <= 4100 and -4100 <= low <= -4050, (high, low))
        self.assertTrue(878 <= crossings <= 882, crossings)   # 440 cycles in one second
        self.assertGreaterEqual(tail, 4050)                  # the amplitude holds to the end

    def test_dma_stops_and_the_amplifier_turns_off(self):
        source = new_file('coachz-prelink-speaker.patch', 'iokit/Kernel/CoachzSpeaker.cpp')
        stop = source.index('clear(kDmaCtl, 1);')
        self.assertLess(stop, source.index('amplifier(false);', stop))
        self.assertIn('0x81200000ull', source)               # a 32-bit DMA address: all kernel memory lies above 4 GiB


if __name__ == '__main__':
    unittest.main()
