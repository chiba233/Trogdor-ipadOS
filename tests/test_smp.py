"""The ADT the SMP route gives XNU: SC7180 cores in MPIDR Affinity1, PSCI by SMC."""
from pathlib import Path
import re
import struct
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from xnu_checkpoint import adt_fields
from xnu_screen import FULL_SCREEN_IDENTITY, Node, apply_identity, read_tree, smp_tree


def boot_tree():
    root = Node()
    root.set('name', b'device-tree\0')
    cpu = root.child('cpus').child('cpu0')
    cpu.set('reg', struct.pack('<I', 0))
    cpu.set('state', b'running\0')
    cpu.set('device_type', b'cpu\0')
    cpu.set('compatible', b'qcom,kryo468\0')
    return root


def props(node):
    return {key: value for key, _, value in node.props}


class SmpTreeTests(unittest.TestCase):
    def test_secondary_cpus_and_psci(self):
        root = boot_tree()
        smp_tree(root, 8)
        root = read_tree(root.write())
        cpus = root.child('cpus').children
        self.assertEqual([c.name() for c in cpus], ['cpu%d' % index for index in range(8)])
        for index, cpu in enumerate(cpus):
            p = props(cpu)
            self.assertEqual(p['reg'], struct.pack('<I', index << 8))
            # XNU reads cluster-type as an integer and compares it with 'E' and 'P'.
            self.assertEqual(p['cluster-type'], struct.pack('<I', ord('P' if index >= 6 else 'E')))
            self.assertEqual(p['state'], b'running\0' if index == 0 else b'waiting\0')
            self.assertEqual(p['device_type'], b'cpu\0')
            self.assertEqual(p['compatible'], b'qcom,kryo468\0')
        psci = props(root.child('psci'))
        # ArmCpu compares the 4-byte method with "smc".
        self.assertEqual(psci['method'], b'smc\0')
        self.assertEqual(psci['compatible'], b'arm,psci-1.0\0')

    def test_checkpoint_fields_come_from_the_running_cpu(self):
        root = boot_tree()
        root.child('chosen').set('dram-base', bytes(8))
        root.child('chosen').set('dram-size', bytes(8))
        root.child('cpus').child('cpu0').set('timebase-frequency', struct.pack('<I', 19200000))
        smp_tree(root, 4)
        self.assertEqual(adt_fields(root.write())[2:], (0, 19200000))
        for cpu in root.child('cpus').children:
            cpu.set('state', b'running\0')
        with self.assertRaises(ValueError):
            adt_fields(root.write())

    def test_all_cores_share_one_pset(self):
        # The single-cluster Clutch scheduler panics in thread_select() with a second pset.
        patch = (Path(__file__).resolve().parents[1] / 'platform/coachz-prelink/patches/xnu/coachz-prelink-smp.patch').read_text()
        self.assertIn('+\tpset = pset_find(0, NULL);', patch)

    def test_kernel_inline_assembly_has_no_semicolons(self):
        # On Mach-O AArch64 ";" starts a comment, so the rest of the string silently disappears. That is
        # how signalCPU lost its ICC_SGI1R write: "dsb ishst; msr icc_sgi1r_el1, %0; isb" built as one dsb.
        root = Path(__file__).resolve().parents[1]
        strings = re.compile(r'(?:__asm__|\basm\b)[^"]*\("([^"]*)"')
        patch = (root / 'platform/coachz-screen/patches/drivers/ArmCpu.patch').read_text()
        console = (root / 'platform/coachz-screen/patches/xnu/coachz-screen-console.patch').read_text()
        for line in (patch + console).splitlines():
            if line.startswith('+'):
                for text in strings.findall(line):
                    self.assertNotIn(';', text, line)
        self.assertIn(r'"dsb ishst\nmsr icc_sgi1r_el1, %0\nisb"', patch)
        upstream = root / 'upstream/unixnu/drivers/ArmCpu/ArmCpu.cpp'
        if upstream.is_file():
            removed = {line[1:] for line in patch.splitlines() if line.startswith('-')}
            for line in upstream.read_text().splitlines():
                if any(';' in text for text in strings.findall(line)):
                    self.assertIn(line, removed)

    def test_threads_look_for_big_cores_first(self):
        patch = (Path(__file__).resolve().parents[1] / 'platform/coachz-prelink/patches/xnu/coachz-prelink-smp.patch').read_text()
        self.assertIn('+		bit_set(pset->perfcontrol_cpu_preferred_bitmask, this_cpu_datap->cpu_number);', patch)
        self.assertIn('+	int rotid = lsb_first(primary_map & pset->perfcontrol_cpu_preferred_bitmask);', patch)
        self.assertIn('+					if (!better_idle && ', patch)

    def test_usb_polling_does_not_block(self):
        # A short IODelay blocks here: every xHCI poll would cost a timer interrupt and a wake from idle.
        source = (Path(__file__).resolve().parents[1] / 'platform/coachz-prelink/drivers/RamBlock/coachz_usb.c').read_text()
        body = source.split('void udelay(unsigned us) {', 1)[1].split('\n}', 1)[0]
        self.assertNotIn('IODelay', body)
        self.assertIn('mach_absolute_time', body)

    def test_frequency_domains_come_from_the_device_tree(self):
        patch = (Path(__file__).resolve().parents[1] / 'platform/coachz-screen/patches/drivers/ArmCpu.patch').read_text()
        self.assertIn('kFreqWindow = 0x18320000', patch)
        self.assertIn('kFreqOffset[3] = {0x3000, 0x5800, 0x1000}', patch)
        self.assertIn('write32 (base, 0x920, best);', patch)

    def test_full_screen_identity_has_no_home_button(self):
        root = Node()
        root.set('compatible', b'J120AP\0')
        apply_identity(root, FULL_SCREEN_IDENTITY['root'])
        for name in ('product', 'buttons'):
            apply_identity(root.child(name), FULL_SCREEN_IDENTITY[name])
        props = {key: value for key, _, value in root.props}
        self.assertEqual(props['model'], b'iPad8,1\0')
        self.assertEqual(props['target-type'], b'J317\0')
        self.assertEqual(props['compatible'], b'J120AP\0')
        buttons = {key: value for key, _, value in root.child('buttons').props}
        self.assertEqual(buttons['home-button-type'], struct.pack('<I', 2))
        self.assertEqual(buttons['button-names'], b'hold\0voldown\0volup\0')
        product = {key: value for key, _, value in root.child('product').props}
        self.assertEqual(product['artwork-device-subtype'], struct.pack('<I', 2388))

    def test_diagnostics_never_use_group_0_gic_registers(self):
        # UniXNU's armcpu_sgi_selfprobe uses them, and on this firmware the machine resets.
        root = Path(__file__).resolve().parents[1] / 'platform'
        report = (root / 'coachz-prelink/patches/xnu/coachz-prelink-launch-diagnostic.patch').read_text()
        self.assertNotIn('armcpu_sgi_selfprobe(', report)
        self.assertIn('armcpu_group1_sgi_probe(out);', report)
        probe = (root / 'coachz-screen/patches/drivers/ArmCpu.patch').read_text()
        start = probe.index('+extern "C" void armcpu_group1_sgi_probe')
        body = probe[start:probe.index('+extern "C" void armcpu_sgi_selfprobe', start) if '+extern "C" void armcpu_sgi_selfprobe' in probe[start:] else None]
        for register in ('icc_sgi0r', 'icc_hppir0', 'icc_iar0', 'icc_eoir0', 'icc_igrpen0'):
            self.assertNotIn(register, body.split('void armcpu_group1_sgi_probe', 1)[1].split('msr daif, %0', 1)[0])

    def test_single_cpu_tree_is_unchanged(self):
        root = boot_tree()
        before = root.write()
        smp_tree(root, 1)
        self.assertEqual(root.write(), before)


if __name__ == '__main__':
    unittest.main()
