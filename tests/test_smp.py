"""The ADT the SMP route gives XNU: SC7180 cores in MPIDR Affinity1, PSCI by SMC."""
from pathlib import Path
import struct
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from xnu_checkpoint import adt_fields
from xnu_screen import Node, read_tree, smp_tree


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
        smp_tree(root, 4)
        root = read_tree(root.write())
        cpus = root.child('cpus').children
        self.assertEqual([c.name() for c in cpus], ['cpu0', 'cpu1', 'cpu2', 'cpu3'])
        for index, cpu in enumerate(cpus):
            p = props(cpu)
            self.assertEqual(p['reg'], struct.pack('<I', index << 8))
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

    def test_single_cpu_tree_is_unchanged(self):
        root = boot_tree()
        before = root.write()
        smp_tree(root, 1)
        self.assertEqual(root.write(), before)


if __name__ == '__main__':
    unittest.main()
