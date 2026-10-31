from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from read_usb import read_usb_record
from usb_stack import runtime_table, unpacked_registers


def record(**changes):
    fields = {'entry EL': 2, 'current EL': 1, 'stage': 3, 'handoff EL': 2, 'USB A controller': 1, 'USB A USBSTS poll': 0x10,
              'USB A disk': 1, 'USB A speed': 3, 'USB A blocks': 2000000, 'USB A block size': 512, 'USB A lba0 rc': 0,
              'USB A lba0 tail': 0xaa55 << 48, 'USB A lba1 rc': 0, 'USB A lba1 head': int.from_bytes(b'EFI PART', 'little'),
              'USB A lba2048 rc': 0, 'USB A lba2048 head': int.from_bytes(b'CHROMEOS', 'little'), 'USB A SMMU sGFSR': 0,
              'USB SMMU sGFSR': 0, 'USB fatal count': 0, 'USB done': 1}
    fields.update({key.replace('_', ' '): value for key, value in changes.items()})
    text = 'Trogdor-ipadOS native phase-0 probe\n' + ''.join(f'{key}=0x{value:016x}\n' for key, value in fields.items())
    return text.encode().ljust(8192, b'\0')


class UsbReadTests(unittest.TestCase):
    def test_hardware_verified_record_is_accepted(self):
        self.assertEqual(read_usb_record(record())[1]['USB A blocks'], 2000000)

    def test_controller_error_or_wrong_data_is_rejected(self):
        for change in ({'USB_A_USBSTS_poll': 0x1015}, {'USB_A_disk': 0}, {'USB_A_lba1_head': 0}, {'USB_A_lba2048_head': 0}):
            with self.assertRaises(ValueError):
                read_usb_record(record(**change))

    def test_registers_stay_32_bit_on_a_strict_alignment_build(self):
        text = ''.join('} __packed ' + member + '\n' for member in ('*capreg;', 'prs[];', '*opreg;', 'intrrs[];', '*hcrreg;'))
        self.assertNotIn('__packed', unpacked_registers(text))
        with self.assertRaises(ValueError):
            unpacked_registers('} *opreg;')
        source = (ROOT / 'boot/usb_read.c').read_text()
        self.assertIn('OFFSET(opreg, dcbaap_lo) == 0x30', source)
        self.assertIn('cbw_direction_data_in', source)
        self.assertNotIn('cbw_direction_data_out', source)

    def test_function_pointer_tables_are_filled_at_run_time(self):
        table = 'static const generic_hub_ops_t x = {\n\t.a\t= f,\n\t.b\t= NULL,\n};\n'
        text = runtime_table(table + '\tif (init(&x))\n', 'x', 'init(&x)', '\tif (')
        self.assertIn('static generic_hub_ops_t x;', text)
        self.assertIn('\tx.a = f;\n\tif (init(&x))', text)
        self.assertNotIn('NULL', text)


if __name__ == '__main__':
    unittest.main()
