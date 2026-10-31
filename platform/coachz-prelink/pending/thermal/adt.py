"""Optional physical-address ADT node; not imported by the active loader."""
import struct


def add_thermal_sampler(root):
    node = root.child('arm-io').child('coachz-thermal')
    node.set('compatible', b'google,coachz-tsens-v2\0')
    # Physical u64 base/size pairs: TM0, SROT0, TM1, SROT1. Not arm-io offsets.
    node.set('reg', struct.pack('<8Q', 0x0c263000, 0x200, 0x0c222000, 0x200,
                               0x0c265000, 0x200, 0x0c223000, 0x200))
