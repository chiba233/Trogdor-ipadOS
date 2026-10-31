"""Host checks of the Adreno 618 stages (coachz-prelink-gpu.patch, tools/xnu_screen.py)."""
import importlib.util
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests'))
from test_battery_speaker import new_file  # noqa: E402

PATCH = 'coachz-prelink-gpu.patch'


class SequenceTests(unittest.TestCase):
    def test_model_runs(self):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            (work / 'coachz_gpu_seq.h').write_text(new_file(PATCH, 'bsd/dev/coachz_gpu_seq.h'))
            (work / 'coachz_gpu_gmu.h').write_text(new_file(PATCH, 'bsd/dev/coachz_gpu_gmu.h'))
            binary = work / 'gpu-model'
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function', '-I', str(work),
                            str(ROOT / 'tests/coachz_gpu_model_test.c'), '-o', str(binary)], check=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout)
            self.assertIn('PASS', result.stdout)

    def test_register_facts(self):
        source = new_file(PATCH, 'bsd/dev/coachz_gpu_seq.h')
        # SC7180 values (sc7180.dtsi, gcc-sc7180.c, gpucc-sc7180.c); SM7150 keeps the GPLL0 gates at 0x52004 instead.
        for fact in ['GPU_CC_BASE 0x05090000u', 'GPU_GMU_BASE 0x0506a000u', 'GCC_BRANCH_VOTE 0x52000u', 'GPU_GPLL0_DIV_GATE (1u << 16)',
                     'GCC_DDRSS_GPU_AXI 0x4452cu', 'GCC_GPU_MEMNOC_GFX 0x7100cu', 'GPUCC_CX_GDSCR 0x106cu',
                     'GPUCC_CX_GDS_HW_CTRL 0x1540u', 'RCG_GMU_200MHZ ((6u << 8) | 2u)', 'GMU_CM3_SYSRESET 0x14000u',
                     'GMU_AO_HOST_INTERRUPT_STATUS 0x24c14u']:
            self.assertIn(fact, source)
        # a6xx_gmu.xml dword offsets relative to the GPU base 0x05000000, as bytes from the GMU base.
        for dword, name in [(0x1f800, 'GMU_CM3_SYSRESET'), (0x1f81c, 'GMU_CM3_FW_INIT_RESULT'), (0x1f82d, 'GMU_CM3_CFG'),
                            (0x1f9c5, 'GMU_GENERAL_0'), (0x23b05, 'GMU_AO_HOST_INTERRUPT_STATUS')]:
            self.assertIn('%s 0x%xu' % (name, dword * 4 - 0x6a000), source)

    def test_no_power_votes_or_core_access(self):
        source = new_file(PATCH, 'bsd/dev/coachz_gpu_seq.h') + new_file(PATCH, 'bsd/dev/coachz_gpu.c')
        self.assertEqual(source.count('GPUCC_GX_GDSCR'), 2)   # its definition and the read list: GX is only read
        for absent in ['rpmh_', '0x05000000u']:   # no RPMh client, no GPU core registers
            self.assertNotIn(absent, source)


    def test_tcm_layout(self):
        source = new_file(PATCH, 'bsd/dev/coachz_gpu_seq.h')
        # GMU_CM3_ITCM_START 0x1b400 and GMU_CM3_DTCM_START 0x1c400: 32 KiB from GMU+0x3000, DTCM right after ITCM.
        self.assertIn('GMU_CM3_ITCM 0x%xu' % (0x1b400 * 4 - 0x6a000), source)
        self.assertEqual(0x1c400 * 4 - 0x6a000, 0x3000 + 0x4000)
        self.assertIn('GMU_TCM_BYTES 0x8000u', source)
        # The only GMU writes are the image words, after CM3 was seen in reset.
        self.assertEqual(source.count('gpu_w(io, GPU_GMU,'), 1)
        self.assertIn('if (r->gmu[0] & 1u) {', source)


    def test_smmu_inventory_only_reads(self):
        source = new_file(PATCH, 'bsd/dev/coachz_gpu_seq.h')
        # sc7180.dtsi adreno_smmu: 64 KiB at 0x05040000, power domain CX GDSC, clocks memnoc (bus) and cfg-ahb (iface).
        self.assertIn('GPU_SMMU_BASE 0x05040000u', source)
        self.assertIn('GPU_SMMU_SIZE 0x00010000u', source)
        self.assertNotIn('gpu_w(io, GPU_SMMU', source)
        reader = source[source.index('static inline void gpu_read_smmu('):source.index('static inline void coachz_gpu_run(')]
        self.assertNotIn('gpu_w(', reader)
        # Read inside the CX window, after the bus branches, never in mode 2 or 3.
        run = source[source.index('static inline void coachz_gpu_run('):]
        self.assertLess(run.index('r->own_memnoc = 1;'), run.index('gpu_read_smmu(io, r);'))
        self.assertLess(run.index('gpu_read_smmu(io, r);'), run.index('gpu_rollback(io, r);\n\tr->stage = r->restored'))
        self.assertIn('if (mode == 4) {\n\t\tgpu_read_smmu(io, r);', run)


class GmuStartTests(unittest.TestCase):
    def test_register_facts(self):
        source = new_file(PATCH, 'bsd/dev/coachz_gpu_gmu.h')
        # a6xx_gmu.xml dword offsets * 4 from the GMU base, and a6xx.xml's PDC domain * 4.
        for dword, name in [(0x5001, 'GMU_CM3_BOOT_CONFIG'), (0x5185, 'GMU_HFI_QTBL_ADDR'), (0x5184, 'GMU_HFI_QTBL_INFO'), (0x5100, 'GMU_PWR_COL_CP_MSG'),
                            (0x51cc, 'GMU_GENERAL_7'), (0x9307, 'GMU_RSCC_CONTROL_REQ'), (0x9311, 'GMU_AHB_FENCE_RANGE_0'), (0x50e8, 'GMU_RPMH_CTRL'),
                            (0x1140, 'PDC_ENABLE'), (0x1148, 'PDC_SEQ_START_ADDR'), (0x1575, 'PDC_TCS1_CMD0'), (0x15d9, 'PDC_TCS3_CMD0')]:
            self.assertIn('%s 0x%xu' % (name, dword * 4), source)
        # a618 uses this level resource and sc7180.dtsi gives the GMU stream 5.
        self.assertIn('PDC_ADDR_GFX 0x30090u', source)
        self.assertIn('GMU_STREAM 5u', source)

    def test_the_firmware_needs_the_checked_bank(self):
        run = new_file(PATCH, 'bsd/dev/coachz_gpu_seq.h')
        self.assertIn('if (gmu_map(io, r) && mode == 6) {', run)
        source = new_file(PATCH, 'bsd/dev/coachz_gpu_gmu.h')
        start = source[source.index('static inline void gmu_start('):source.index('static inline void gmu_undo(')]
        for later in ['gmu_rsc_program(io, r);', 'gmu_rsc_start(io, r)', 'gpu_load_tcm(io, image, image_bytes, r);', 'gpu_w(io, GPU_GMU, GMU_CM3_SYSRESET, 0);']:
            self.assertIn(later, start)
        self.assertLess(start.index('gpu_load_tcm(io'), start.index('gpu_w(io, GPU_GMU, GMU_CM3_SYSRESET, 0);'))
        # No GX and no HFI message at this stage.
        for absent in ['GPUCC_GX_GDSCR', 'GMU_GX_VOTE', 'HOST2GMU']:
            self.assertNotIn(absent, source)


class LoaderTests(unittest.TestCase):
    def test_firmware_lands_in_kernel_data_after_the_trust_cache(self):
        with tempfile.TemporaryDirectory() as directory:
            sys.path.insert(0, str(ROOT / 'tools'))
            try:
                spec = importlib.util.spec_from_file_location('xnu_screen_gpu', ROOT / 'tools/xnu_screen.py')
                screen = importlib.util.module_from_spec(spec)
                spec.loader.exec_module(screen)
            finally:
                sys.path.remove(str(ROOT / 'tools'))
            screen.OS_PROFILE = ROOT / 'upstream/unixnu/profiles/os/ipados-17.7.11-21H461/profile.toml'
            root = screen.Node()
            root.set('name', b'device-tree\0')
            root.child('chosen').child('memory-map')
            if not screen.OS_PROFILE.is_file():
                self.skipTest('needs the pinned UniXNU OS profile')
            window, tree, ramdisk, cache = 0x80000000, 0x2000000, bytes(0x5000), bytes(0x1234)
            adt = screen.prelink_tree(root.write(), window, tree, ramdisk, cache, gmu_firmware_size=0x8000)
            entries = {key: value for key, _, value in screen.read_tree(adt).child('chosen').child('memory-map').props}
            cache_base, cache_size = struct.unpack('<QQ', entries['TrustCache'])
            base, size = struct.unpack('<QQ', entries['CoachzGmuFirmware'])
            self.assertEqual(cache_size, len(cache))
            self.assertEqual(base, cache_base + 0x2000)   # the trust cache padded to 4 KiB
            self.assertEqual((base % 4096, size), (0, 0x8000))
            with self.assertRaises(ValueError):
                screen.pack(b'', b'', 0, 0, prelinked=True, gpu=3)   # stage 3 needs the firmware
            with self.assertRaises(ValueError):
                screen.pack(b'', b'', 0, 0, prelinked=True, gpu=6)   # and so does stage 6
            with self.assertRaises(ValueError):
                screen.pack(b'', b'', 0, 0, prelinked=True, gpu=2, gmu_firmware=bytes(16))


if __name__ == '__main__':
    unittest.main()
