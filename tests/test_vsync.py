"""Host checks for the panel's hardware vsync (platform/coachz-prelink/drivers/RamBlock/vsync/coachz_vsync.c and its use in
GopFramebuffer.patch). The values are mainline Linux v6.12's (dpu_6_2_sc7180.h, dpu_hwio.h, msm_mdss.c, sc7180.dtsi). Nothing here is a
hardware result."""
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'platform/coachz-prelink/drivers/RamBlock/vsync/coachz_vsync.c'


def define(source, name):
    return int(re.search(r'#define ' + name + r'\s+\(?(0x[0-9a-fA-F]+|\d+)u?', source).group(1), 0)


class VsyncTests(unittest.TestCase):
    def setUp(self):
        self.source = SOURCE.read_text()

    def test_mainline_values(self):
        self.assertEqual(define(self.source, 'MDSS_PA'), 0x0ae00000)
        self.assertEqual(define(self.source, 'MDSS_HW_INTR_STATUS'), 0x10)
        self.assertEqual(define(self.source, 'MDP_PA'), 0x0ae01000)
        self.assertEqual((define(self.source, 'MDP_INTR_EN'), define(self.source, 'MDP_INTR_STATUS'), define(self.source, 'MDP_INTR_CLEAR')),
                         (0x10, 0x14, 0x18))
        self.assertIn('#define INTF_1_VSYNC     (1u << 27)', self.source)
        self.assertEqual(define(self.source, 'MDSS_SPI'), 83)
        self.assertIn('#define MDSS_INTID       (32u + MDSS_SPI)', self.source)

    def test_handler_order_follows_dpu_core_irq(self):
        body = self.source[self.source.index('static void vs_irq('):self.source.index('int coachz_hw_vsync_register(void (*callback)(void *, uint64_t), void *context)\n{')]
        self.assertLess(body.index('rd(vs_mdp, MDP_INTR_STATUS)'), body.index('wr(vs_mdp, MDP_INTR_CLEAR, status)'))
        self.assertLess(body.index('wr(vs_mdp, MDP_INTR_CLEAR, status)'), body.index('status &= enabled'))

    def test_wiring(self):
        build = (ROOT / 'platform/coachz-prelink/drivers/RamBlock/build-usb-stack.py').read_text()
        self.assertIn("vsync / 'coachz_vsync.c'", build)
        patch = (ROOT / 'platform/coachz-prelink/patches/display/GopFramebuffer.patch').read_text()
        self.assertIn('+extern "C" int coachz_hw_vsync_register(void (*callback)(void *,uint64_t),void *context);', patch)
        self.assertIn('+        vsyncHardware=coachz_hw_vsync_register(&NativeLegacyDisplay::hardwareVsync,this)==0;', patch)
        # Only a registered interrupt skips the timer; otherwise the original software tick stays armed as before.
        self.assertIn('+        if(vsyncHardware){vsyncArmed=true;native_legacy_vsync_observed(0,vsyncHz);return;}', patch)
        self.assertNotIn('-        vsyncTimer->wakeAtTime(vsyncNext);native_legacy_vsync_observed(0,vsyncHz);', patch)


if __name__ == '__main__':
    unittest.main()
