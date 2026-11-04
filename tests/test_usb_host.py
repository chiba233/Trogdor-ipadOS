"""Host checks for the Apple USB host stack route (coachz-usb-host=1, docs/plans/usb-host.md). Never builds or boots a kernel."""
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
PACKAGE = ROOT / 'platform/coachz-prelink/package'
KERNELCACHE = ROOT / 'build/xnu-source-prelink/ipsw/21H461/kernelcache.decompressed'
GENERATED = ('OriginalAppleUSBABI.h', 'usb-original-bindings.S', 'usb-original.json')
SOURCE = PACKAGE / 'CoachzUsbHost.cpp'


class UsbHostTests(unittest.TestCase):
    @unittest.skipUnless(KERNELCACHE.is_file(), 'the pinned original kernelcache is not extracted here')
    def test_recorded_files_match_the_original_kernelcache(self):
        with tempfile.TemporaryDirectory(prefix='coachz-usb-abi-') as directory:
            subprocess.run([sys.executable, '-I', str(ROOT / 'tools/record_usb_bindings.py'), str(KERNELCACHE), '--output', directory],
                           check=True, capture_output=True)
            for name in GENERATED:
                self.assertEqual((Path(directory) / name).read_bytes(), (PACKAGE / name).read_bytes(), name)

    def test_recorded_slots_are_consistent(self):
        report = json.loads((PACKAGE / 'usb-original.json').read_text())
        self.assertEqual(report['slide'], -0x1000000)
        self.assertEqual(report['classes']['AppleUSBXHCI']['slots'], 299)
        self.assertEqual(report['classes']['AppleNubT8011USBXHCI']['slots'], 301)
        self.assertEqual(report['classes']['AppleNubT8011USBXHCI']['superclass'], 'AppleUSBXHCI')
        self.assertEqual(report['cache_options_offset'], 0x494)
        self.assertEqual(report['provider_slots'], {'121': '__ZN9IOService24getDeviceMemoryWithIndexEj',
                                                    '127': '__ZN9IOService16getInterruptTypeEiPi'})
        header = (PACKAGE / 'OriginalAppleUSBABI.h').read_text()
        replaced = [int(v) for v in re.search(r'kAppleNubT8011USBXHCIReplaced\[\] = \{([^}]*)\}', header).group(1).split(',')]
        # The destructors, getMetaClass and the register accessors (64-bit writes as two 32-bit writes) stay the subclass's.
        self.assertEqual(replaced, [s for s in report['nub_overrides'] if s not in (0, 1, 7) and s not in report['kept']])
        self.assertEqual(report['kept'], list(range(289, 298)))
        self.assertIn(86, replaced)
        bindings = re.findall(r'^\.set (\S+), (0x[0-9a-f]+)$', (PACKAGE / 'usb-original-bindings.S').read_text(), re.M)
        self.assertEqual([n for n, _ in bindings], ['_coachz_original_xhci_table'])

    def test_controller_and_mapper(self):
        source = SOURCE.read_text()
        self.assertIn('OSMetaClass::allocClassWithName("AppleNubT8011USBXHCI")', source)
        self.assertIn('table[slot] = coachz_original_xhci_table[slot]', source)
        # AppleUSBHostController::start fails without IOMapper::copyMapperForDevice(provider), which reads iommu-parent.
        self.assertIn('nub->setProperty("iommu-parent", mapper)', source)
        self.assertIn('class CoachzUsbMapper : public IOMapper', source)
        self.assertIn('constexpr uint16_t kStream = 0x540;', source)
        # The DWC3 does not snoop: its rings are write-combined, never RealTime (Device memory in this kernel).
        self.assertIn('kIOMapWriteCombineCache', source)
        self.assertNotIn('kIOMapRealTimeCache;', source)
        # Firmware's apps SMMU state is required as found; other streams keep bypassing.
        self.assertIn('if (!(scr0 & kScr0ClientPd))', source)
        self.assertIn('if (gr0(kGr0Smr + 4 * i) & kSmrValid)', source)
        self.assertIn('~(kScr0ClientPd | kScr0Usfcfg | kScr0Fb | kScr0Bsu)', source)
        # Cache maintenance by region only: the set/way path reaches one core.
        self.assertIn('dcache_incoherent_io_flush64(physical, chunk, 0, &result)', source)

    def test_build_and_link(self):
        script = (ROOT / 'platform/coachz-prelink/scripts/build-coachz-usb-host.py').read_text()
        self.assertIn('usb-original-bindings.S', script)
        self.assertIn("the SMMU kit must come from libCoachzGpu.a only", script)
        board = (ROOT / 'platform/coachz-prelink/board.toml').read_text()
        self.assertIn('"build-coachz-usb-host.py"', board)
        self.assertLess(board.index('"build-coachz-gpu.py"'), board.index('"build-coachz-usb-host.py"'))
        for root in ('AppleUSBHostT8011', 'AppleUSBHub', 'AppleUSBHostCompositeDevice', 'IOUSBHostHIDDevice'):
            self.assertIn(f'"--root", "com.apple.driver.usb.{root}"', board)
        self.assertIn('build/coachz-usb-host/libCoachzUsbHost.a', (ROOT / 'tools/kernel_source.py').read_text())

    def test_default_boot_is_unchanged(self):
        series = (ROOT / 'platform/coachz-prelink/patches/xnu/series').read_text().split()
        self.assertIn('coachz-prelink-usb-host.patch', series)
        self.assertEqual(series[-1], 'coachz-prelink-baseband.patch')
        patch = (ROOT / 'platform/coachz-prelink/patches/xnu/coachz-prelink-usb-host.patch').read_text()
        self.assertIn('+++ b/iokit/Kernel/IOCatalogue.cpp', patch)
        self.assertIn('PE_parse_boot_argn("coachz-usb-host"', patch)
        self.assertEqual(set(re.findall(r'isEqualTo\("(\w+)"\)', patch)), {'AppleUSBHostResources', 'AppleUSBHostResourcesTypeC'})
        block = (ROOT / 'platform/coachz-prelink/drivers/RamBlock/NativeRamBlock.cpp').read_text()
        self.assertLess(block.index('if (coachz_usb_host_enabled())'), block.index('coachz_usb_open_keyboard();'))

    def test_image_switch(self):
        screen = (ROOT / 'tools/xnu_screen.py').read_text()
        self.assertIn("command_line += ' coachz-usb-host=1'", screen)
        self.assertNotIn("replace(' coachz-usb-keyboard=1'", screen)
        self.assertIn("the Apple USB host stack needs the eMMC root", screen)
        # The libpayload keyboard route stays off in the kernel itself while Apple's stack owns the controller.
        usb = (ROOT / 'platform/coachz-prelink/drivers/RamBlock/coachz_usb.c').read_text()
        self.assertIn('return coachz_usb_keyboard_requested() && !coachz_usb_host_enabled();', usb)

    def test_base_input_has_one_route(self):
        # The decoding is transport-independent and both transports feed it.
        ram = ROOT / 'platform/coachz-prelink/drivers/RamBlock'
        hid = (ram / 'coachz_usb_hid.c').read_text()
        self.assertIn('coachz_zed_keyboard(report, KEYBOARD_REPORT);', hid)
        self.assertIn('coachz_zed_pad(report, PAD_REPORT);', hid)
        self.assertNotIn('coachz_input_mouse_report(', hid)
        self.assertIn("HERE / 'coachz_zed_input.c'", (ram / 'build-usb-stack.py').read_text())
        source = SOURCE.read_text()
        self.assertIn('coachz_zed_keyboard(bytes', source)
        self.assertIn('coachz_zed_pad(bytes', source)
        # The bridge takes the base's IOHIDInterface from IOHIDEventDriver and opens it with a report action (slot 168, recorded).
        self.assertIn('kZedVendor = 0x18d1, kZedProduct = 0x504c', source)
        self.assertIn('kZedKeyboardInterface = 0, kZedTouchpadInterface = 2', source)
        self.assertIn('[kIOHIDInterfaceOpenSlot]', source)
        self.assertIn('if (coachz_usb_keyboard_requested()) log("base HID bridge personality %d", add_bridge_personality());', source)
        self.assertIn('enum { kIOHIDInterfaceOpenSlot = 168 };', (PACKAGE / 'OriginalAppleUSBABI.h').read_text())


if __name__ == '__main__':
    unittest.main()
