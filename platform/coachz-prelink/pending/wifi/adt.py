"""Optional ADT helper for the pending read-only Wi-Fi inventory. Not imported by the loader."""
import struct


def add_wifi_inventory(root, smem_base, smem_size):
    """Call only after checking the original FDT and excluding SMEM from XNU allocations."""
    if (smem_base, smem_size) != (0x80900000, 0x200000):
        raise ValueError('this inventory supports only the verified SC7180 SMEM layout')
    node = root.child('arm-io').child('coachz-wifi')
    node.set('compatible', b'google,coachz-wcn3990\0')
    node.set('smem-region', struct.pack('<QQ', smem_base, smem_size))
