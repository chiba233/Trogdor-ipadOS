"""Host checks of the not-yet-enabled input patch and the pending USB keyboard file."""
from pathlib import Path
import re
import unittest

from test_battery_speaker import new_file

ROOT = Path(__file__).resolve().parents[1]


class InputTests(unittest.TestCase):
    def test_buttons_come_from_the_current_state_not_the_event_queue(self):
        source = new_file('coachz-prelink-input.patch', 'iokit/Kernel/CoachzInput.cpp')
        self.assertEqual(set(re.findall(r'= coachz_ec_command\((\w+)', source)), {'kMkbpInfo'})
        self.assertIn('const uint16_t kMkbpInfo = 0x0061;', source)
        self.assertNotIn('GET_NEXT_EVENT', source.replace('no MKBP event is taken', ''))

    def test_power_is_opt_in_and_bits_match_the_descriptor(self):
        source = new_file('coachz-prelink-input.patch', 'iokit/Kernel/CoachzInput.cpp')
        # EC bits power 0, volume up 1, volume down 2; NativeHidInput report 2: Power 1, Volume Increment 2, Decrement 3.
        self.assertIn('if ((gEnabled & 2) && (ec & 1)) bits |= 1u << 1;', source)
        self.assertIn('if ((gEnabled & 1) && (ec & 2)) bits |= 1u << 2;', source)
        self.assertIn('if ((gEnabled & 1) && (ec & 4)) bits |= 1u << 3;', source)
        # handleReport is slot 181 of the original IOHIDDevice, as build-sheng-hid-input.py checks.
        self.assertIn('const unsigned kHandleReportSlot = 181;', source)
        script = (ROOT / 'platform/coachz-prelink/scripts/build-sheng-hid-input.py').read_text()
        self.assertIn('methods[181]==0xfffffff004add0c4', script)

    def test_usb_keyboard_never_polls_the_disk(self):
        source = (ROOT / 'platform/coachz-prelink/pending/usb-keyboard/coachz_usb_hid.c').read_text()
        code = re.sub(r'/\*.*?\*/', '', source, flags=re.S)
        self.assertNotIn('usb_poll(', code)
        self.assertIn('dev != disk', code)
        # Boot protocol first: an 8-byte reading is only valid after SET_PROTOCOL(boot).
        self.assertLess(code.index('HID_SET_PROTOCOL, HID_BOOT_PROTOCOL'), code.index('create_intr_queue(in, 8, 8'))


if __name__ == '__main__':
    unittest.main()
