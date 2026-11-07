"""Host checks for the video codec service (package/CoachzVenus.cpp) and its VideoToolbox decoder (package/CoachzVideoDecoder.c).

These cover what runs off the device: the mainline-derived constants, the ABI both sides compile against, the delivery wiring, and the
decoder's build. Nothing here is a hardware result."""
import math
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PACKAGE = ROOT / 'platform/coachz-prelink/package'
PATCHES = ROOT / 'platform/coachz-prelink/patches/xnu'


class VenusTests(unittest.TestCase):
    def setUp(self):
        self.kernel = (PACKAGE / 'CoachzVenus.cpp').read_text()
        self.decoder = (PACKAGE / 'CoachzVideoDecoder.c').read_text()
        self.abi = (PACKAGE / 'CoachzVideoAbi.h').read_text()

    def test_opp_table_follows_videocc_and_alpha_pll(self):
        # ftbl_video_cc_venus_clk_src: PLL / 4, / 2.5, / 2, / 2, / 2; alpha_pll_round_rate: L, then the 16-bit alpha rounded up.
        rows = re.findall(r'\{(\d+), (\d+), 0x([0-9a-f]+), (\d+), (\d+)\},', self.kernel)
        self.assertEqual(len(rows), 5)
        dividers = {150000000: 4, 270000000: 2.5, 340000000: 2, 434000000: 2, 500000000: 2}
        corners = {150000000: 64, 270000000: 128, 340000000: 192, 434000000: 256, 500000000: 384}
        for hz, l, alpha, field, cx in rows:
            hz, l, alpha, field, cx = int(hz), int(l), int(alpha, 16), int(field), int(cx)
            pll = round(hz * dividers[hz])
            ref = 19200000
            self.assertEqual(l, pll // ref, hz)
            self.assertEqual(alpha, math.ceil((pll % ref) * 65536 / ref), hz)
            self.assertEqual(field, int(2 * dividers[hz] - 1), hz)
            self.assertEqual(cx, corners[hz], hz)

    def test_hfi_numbers_match_mainline(self):
        for name, value in (('kCmdSessionInit', '0x10007'), ('kCmdFlush', '0x211008'), ('kCmdContinue', '0x21100d'),
                            ('kMsgFillBuffer', '0x221008'), ('kMsgFlush', '0x221006'), ('kPropBufreq', '0x202001'),
                            ('kEventSeqChanged', '0x1000003'), ('kFlushAll', '0x1000004'), ('kIndexInputCrop', '0x0700000e')):
            self.assertRegex(self.kernel, rf'{name} = {value}\b')
        self.assertIn('armcpu_spi_attach(32 + 174, false', self.kernel)       # GIC_SPI 174, IRQ_TYPE_LEVEL_HIGH
        self.assertIn('kDynEnd = kSharedIova', self.kernel)
        self.assertIn('kSharedIova = 0xc0000000', self.kernel)                 # below sc7180_res.dma_mask
        self.assertIn('etb(s, 0xdeadb000u, 0, 0, kFlagEos', self.kernel)       # vdec_decoder_cmd's EOS address
        # hfi_msgs.c event_session_error: these four are not fatal.
        self.assertIn('error == 0x1012 || error == 0x1010 || error == 0x1008 || error == 0x1013', self.kernel)
        # pkt_session_set_property_4xx: BUFFER_ALLOC_MODE (0x201008) is -ENOTSUPP, never sent on 4xx.
        self.assertNotIn('0x201008', self.kernel)
        # intbufs_set_buffer: SET_BUFFERS carries bufreq.size.
        self.assertIn('{36, kCmdSessionSetBuffers, s->id, type, r->size, 0, r->size, 1, b->iova}', self.kernel)
        self.assertIn('fsr & 0xc00001feu', self.kernel)

    def test_both_sides_share_the_abi(self):
        self.assertIn('#include "CoachzVideoAbi.h"', self.kernel)
        self.assertIn('#include "CoachzVideoAbi.h"', self.decoder)
        if shutil.which('xcrun'):
            # The kernel and the decoder are both arm64 LP64: the host compiler gives the same layout.
            with tempfile.TemporaryDirectory() as directory:
                probe = Path(directory) / 'abi.c'
                probe.write_text('#include <stddef.h>\n#include <stdio.h>\n#include "CoachzVideoAbi.h"\nint main(void) {printf("%zu %zu %zu %zu\\n", '
                                 'sizeof(CoachzVideoEvent), offsetof(CoachzVideoEvent, token), sizeof(CoachzVideoInfo), '
                                 'offsetof(CoachzVideoEvent, crop_height)); return 0;}\n')
                subprocess.run(['xcrun', 'clang', '-I', str(PACKAGE), str(probe), '-o', str(Path(directory) / 'abi')], check=True)
                layout = subprocess.run([str(Path(directory) / 'abi')], capture_output=True, text=True, check=True).stdout.split()
                self.assertEqual(layout, ['80', '16', '16', '72'])
        self.assertIn('setProperty("IOUserClientClass", "CoachzVenusUserClient")', self.kernel)
        self.assertIn('#define COACHZ_VIDEO_CLASS "CoachzVenus"', self.abi)

    def test_delivery(self):
        series = [line for line in (PATCHES / 'series').read_text().splitlines() if line and not line.startswith('#')]
        self.assertEqual(series[-1], 'coachz-prelink-video-decoder.patch')
        patch = (PATCHES / 'coachz-prelink-video-decoder.patch').read_text()
        self.assertIn('"/System/Library/VideoDecoders"', patch)
        self.assertIn('+		coachz_video_prepare(p, scratch);', patch)
        self.assertIn('"CoachzVenusUserClient"', (PATCHES / 'coachz-prelink-gpu-map-grant.patch').read_text())
        build = (ROOT / 'platform/coachz-prelink/scripts/build-coachz-venus.py').read_text()
        self.assertIn("'-install_name', '/System/Library/VideoDecoders/AVD.videodecoder'", build)
        self.assertIn("stage_initializer(raw, '__GLOBAL__sub_I_CoachzVenus.cpp')", build)
        self.assertIn('setProperty("NativeSandboxGrantProcess", "*")', self.kernel)

    def test_decoder_class_layout(self):
        # kParavirtualizedVideoDecoder_VideoDecoderClass: version 3, ten slots; the base class version 1.
        self.assertIn('static const DecoderClass kClass = {3, start_session, decode_frame, copy_supported, NULL, NULL, can_accept, '
                      'finish_delayed_frames,', self.decoder)
        self.assertIn('static const DecoderBaseClass kBase = {1, sizeof(Decoder), NULL, invalidate, finalize,', self.decoder)
        self.assertIn("VTRegisterVideoDecoder(codecs[i], create_instance)", self.decoder)

    @unittest.skipUnless(shutil.which('xcrun'), 'needs Xcode')
    def test_decoder_builds_and_exports_register(self):
        sdk = subprocess.run(['xcrun', '--sdk', 'iphoneos', '--show-sdk-path'], capture_output=True, text=True)
        if sdk.returncode:
            self.skipTest('no iOS SDK')
        with tempfile.TemporaryDirectory() as directory:
            out = Path(directory) / 'AVD.videodecoder'
            subprocess.run(['xcrun', 'clang', '-target', 'arm64-apple-ios17.0', '-isysroot', sdk.stdout.strip(), '-I', str(PACKAGE), '-O2',
                            '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter', '-dynamiclib', '-fvisibility=hidden',
                            '-framework', 'CoreFoundation', '-framework', 'CoreMedia', '-framework', 'CoreVideo', '-framework',
                            'VideoToolbox', '-framework', 'IOKit', str(PACKAGE / 'CoachzVideoDecoder.c'), '-o', str(out)], check=True)
            exported = subprocess.run(['xcrun', 'nm', '-gU', str(out)], capture_output=True, text=True, check=True).stdout.split()
            self.assertEqual([name for name in exported if name.startswith('_')], ['_AVDRegister'])


if __name__ == '__main__':
    unittest.main()
