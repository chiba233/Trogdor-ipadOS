"""Host checks of the system audio path: what the kernel service, the plug-in and the image must agree on."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
PACKAGE = ROOT / 'platform/coachz-prelink/package'


class AudioTests(unittest.TestCase):
    def test_the_plug_in_is_the_coachz_one(self):
        source = (PACKAGE / 'CoachzAudioPlugIn.c').read_text()
        self.assertIn('IOServiceMatching(COACHZ_AUDIO_CLASS)', source)
        self.assertIn('org.trogdor.coachz.audio', (PACKAGE / 'CoachzAudioPlugIn.plist').read_text())
        self.assertEqual(source.count('Sheng'), 1)   # only the note of where it came from

    def test_the_output_ring_is_normal_memory_the_dma_can_address(self):
        source = (PACKAGE / 'CoachzAudio.cpp').read_text()
        self.assertIn('const uint64_t kRingPa = 0x81200000;', source)
        self.assertIn('kIOMapWriteCombineCache', source)   # the audio server mixes with unaligned accesses

    def test_the_service_is_allowed_through_the_sandbox(self):
        self.assertIn('"NativeSandboxGrantProcess", "audiomxd,mediaserverd"', (PACKAGE / 'CoachzAudio.cpp').read_text())

    def test_the_plug_in_is_installed_only_with_the_service(self):
        patch = (ROOT / 'platform/coachz-prelink/patches/xnu/coachz-prelink-audio.patch').read_text()
        self.assertIn('mode != 2', patch)
        self.assertIn('coachz-speaker=2', (ROOT / 'tools/xnu_screen.py').read_text())


if __name__ == '__main__':
    unittest.main()
