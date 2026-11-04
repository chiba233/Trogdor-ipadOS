"""Host checks for the Wi-Fi registry on the original IONetworkingFamily classes. Never builds or boots a kernel."""
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
GENERATED = ('OriginalIONetworkingABI.h', 'network-original-bindings.S', 'network-original.json')


class NetworkBindingTests(unittest.TestCase):
    def bindings(self):
        text = (PACKAGE / 'network-original-bindings.S').read_text()
        return dict((name, int(value, 16)) for name, value in re.findall(r'^\.set (\S+), (0x[0-9a-f]+)$', text, re.M))

    @unittest.skipUnless(KERNELCACHE.is_file(), 'the pinned original kernelcache is not extracted here')
    def test_recorded_files_match_the_original_kernelcache(self):
        with tempfile.TemporaryDirectory(prefix='coachz-net-abi-') as directory:
            subprocess.run([sys.executable, '-I', str(ROOT / 'tools/record_network_bindings.py'), str(KERNELCACHE), '--output', directory],
                           check=True, capture_output=True)
            for name in GENERATED:
                self.assertEqual((Path(directory) / name).read_bytes(), (PACKAGE / name).read_bytes(), name)

    def test_recorded_bindings_are_consistent(self):
        report = json.loads((PACKAGE / 'network-original.json').read_text())
        self.assertEqual(report['slide'], -0x1000000)
        bindings = self.bindings()
        own = 0
        for cls, spec in report['classes'].items():
            prefix = f'{len(cls)}{cls}'
            if not spec['bound']:
                self.assertFalse(any(prefix in name for name in bindings), cls)
                continue
            for name in (f'__ZN{prefix}C2EPK11OSMetaClass', f'__ZN{prefix}D2Ev', f'__ZN{prefix}10gMetaClassE', f'__ZTV{prefix}'):
                self.assertIn(name, bindings)
            self.assertNotIn(f'__ZN{prefix}D0Ev', bindings)
            for slot in range(168, spec['slots']):
                self.assertEqual(f'__ZN{prefix}11original{slot}Ev' in bindings, slot not in spec['pure'])
                own += slot not in spec['pure']
            # The base destructor and the protected constructor are code; the metaclass and the table are data, at the slide.
            for name in (f'__ZN{prefix}C2EPK11OSMetaClass', f'__ZN{prefix}D2Ev'):
                self.assertTrue(0xfffffff004920000 <= bindings[name] < 0xfffffff004936494, name)
        self.assertEqual(len(bindings), 8 + own)
        header = (PACKAGE / 'OriginalIONetworkingABI.h').read_text()
        self.assertIn('static_assert(sizeof(IONetworkInterface) == 0x110', header)
        self.assertIn('static_assert(sizeof(IONetworkStack) == 0xd0', header)
        self.assertIn('virtual void original178() = 0;', header)
        self.assertIn('static const unsigned short kIONetworkStackUserClientReplaced[] = {44, 86, 170, 172, 173};', header)
        self.assertEqual(report['classes']['IONetworkStackUserClient']['base'], 'IOUserClient')

    def test_registry_no_longer_defines_family_classes(self):
        source = (PACKAGE / 'CoachzWifiNet.cpp').read_text()
        for name in ('IONetworkInterface', 'IONetworkStack', 'IONetworkStackUserClient'):
            self.assertNotIn(f'OSDefineMetaClassAndStructors({name},', source)
        # configd's sandbox opens a stack user client only by its original class name.
        self.assertIn('OSMetaClass::allocClassWithName("IONetworkStackUserClient")', source)
        self.assertNotIn('OSTypeAlloc(CoachzNetworkStackUserClient)', source)
        self.assertIn('OSDefineMetaClassAndStructors(IO80211Interface, IONetworkInterface)', source)
        self.assertIn('OSDefineMetaClassAndStructors(CoachzNetworkStack, IONetworkStack)', source)
        self.assertIn('OSKextLoadKextWithIdentifier("com.apple.iokit.IONetworkingFamily")', source)

    def test_family_is_prelinked_and_its_stack_personality_removed(self):
        board = (ROOT / 'platform/coachz-prelink/board.toml').read_text()
        self.assertIn('"--root", "com.apple.iokit.IONetworkingFamily"', board)
        series = (ROOT / 'platform/coachz-prelink/patches/xnu/series').read_text().split()
        self.assertIn('coachz-prelink-network-family.patch', series)
        patch = (ROOT / 'platform/coachz-prelink/patches/xnu/coachz-prelink-network-family.patch').read_text()
        self.assertIn('+++ b/iokit/bsddev/IOKitBSDInit.cpp', patch)
        self.assertLess(patch.index('removeDrivers'), patch.index(' \tIOService::publishResource("IOBSD");'))

    def test_build_checks_the_compiled_tables(self):
        script = (ROOT / 'platform/coachz-prelink/scripts/build-coachz-wifi-net.py').read_text()
        self.assertIn("'IO80211Interface': 'IONetworkInterface'", script)
        self.assertIn("'CoachzNetworkStack': 'IONetworkStack'", script)
        self.assertIn('network-original-bindings.S', script)


if __name__ == '__main__':
    unittest.main()
