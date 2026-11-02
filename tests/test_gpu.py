"""Host checks of the Adreno 618 stages (coachz-prelink-gpu.patch, tools/xnu_screen.py)."""
import importlib.util
import re
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
            (work / 'coachz_gpu_core.h').write_text(new_file(PATCH, 'bsd/dev/coachz_gpu_core.h'))
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

    def test_runtime_power_follows_mainline_and_gates_core_access(self):
        core = new_file(PATCH, 'bsd/dev/coachz_gpu_core.h')
        body = core[core.index('static inline int gpu_pm_slumber('):]
        # a6xx_gmu_shutdown: CP and bus first, GPU_SET off, SPTP off, the slumber OOB, HFI stopped, CM3 in reset, the RSC stopped, GX collapsed.
        order = [body.index(s) for s in ('core_stop(io, r);', '1u << GMU_OOB_GPU_ACK', 'GMU_GX_SPTPRAC_POWER_CONTROL, 0x778001u', 'GMU_BOOT_SLUMBER_OPTION, 1u',
                                         '1u << GMU_OOB_BOOT_SET', 'GMU_HFI_CTRL_INIT, 0', 'GMU_CM3_SYSRESET, 1u', 'gmu_rsc_stop(io)', 'GDSC_SW_COLLAPSE')]
        self.assertEqual(order, sorted(order))
        board = (ROOT / 'platform/coachz-prelink/package/CoachzGpu.cpp').read_text()
        # With GX off a core register access stops the machine: both accessors pass the gate, and sleeping waits for its users.
        self.assertEqual(board.count('if (!pmEnter())'), 2)
        self.assertLess(board.index('__atomic_store_n(&gPmAsleep, 1u'), board.index('while (__atomic_load_n(&gPmUsers'))
        self.assertLess(board.index('while (__atomic_load_n(&gPmUsers'), board.index('const int status = coachz_gpu_pm_suspend();'))
        # Only after the power button, and the first submission wakes it.
        self.assertIn('if (!gPmArmed || gPmAsleep', board)
        ring = (ROOT / 'platform/coachz-prelink/patches/gpu/Adreno740.patch').read_text()
        self.assertLess(ring.index('+    if(native_adreno_power_get)native_adreno_power_get();'), ring.index('     status=ring_->submitFramed(words,word_count,&value);'))

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
        self.assertIn('if (gmu_map(io, r) && mode >= 6) {', run)
        source = new_file(PATCH, 'bsd/dev/coachz_gpu_gmu.h')
        start = source[source.index('static inline void gmu_start('):source.index('static inline void gmu_undo(')]
        for later in ['gmu_rsc_program(io, r);', 'gmu_rsc_start(io, r)', 'gpu_load_tcm(io, image, image_bytes, r);', 'gpu_w(io, GPU_GMU, GMU_CM3_SYSRESET, 0);']:
            self.assertIn(later, start)
        self.assertLess(start.index('gpu_load_tcm(io'), start.index('gpu_w(io, GPU_GMU, GMU_CM3_SYSRESET, 0);'))
        # GX and HFI only from mode 7, after the init result, and never the GPU core's own registers.
        self.assertIn('if (r->mode >= 7 && r->fw_init_polls) gmu_bring_up_gx(io, r);', start)
        self.assertEqual(start.count('gmu_bring_up_gx(io, r)'), 1)
        self.assertNotIn('0x05000000u', source)

    def test_gx_and_hfi_facts(self):
        source = new_file(PATCH, 'bsd/dev/coachz_gpu_gmu.h')
        # a6xx_gmu.xml dword offsets * 4, the legacy OOB bits, and a6xx_hfi.h's message sizes.
        for dword, name in [(0x5194, 'GMU_HOST2GMU_INTR_SET'), (0x5191, 'GMU_GMU2HOST_INTR_CLR'), (0x81, 'GMU_GX_SPTPRAC_POWER_CONTROL'),
                            (0x23f8, 'GMU_BOOT_SLUMBER_OPTION'), (0x23f9, 'GMU_GX_VOTE_IDX'), (0x23fa, 'GMU_MX_VOTE_IDX')]:
            self.assertIn('%s 0x%xu' % (name, dword * 4), source)
        for fact in ['GMU_OOB_BOOT_SET 22u', 'GMU_OOB_BOOT_ACK 30u', 'GMU_OOB_GPU_SET 16u', 'GMU_OOB_GPU_ACK 24u', 'HFI_PERF_DWORDS 43u', 'HFI_BW_DWORDS 160u']:
            self.assertIn(fact, source)
        self.assertEqual(1 + 2 + 2 * 16 + 2 * 4, 43)
        self.assertEqual(6 + 6 + 2 * 6 + 8 + 16 * 8, 160)


class PatchTests(unittest.TestCase):
    def test_new_file_hunks_count_their_lines(self):
        text = (ROOT / 'platform/coachz-prelink/patches/xnu' / PATCH).read_text()
        for name, count, body in re.findall(r'^\+\+\+ b/(\S+)\n@@ -0,0 \+1,(\d+) @@\n((?:\+.*\n)*)', text, re.M):
            self.assertEqual(int(count), body.count('\n'), name)


class DriverTests(unittest.TestCase):
    def test_the_a740_framework_is_patched_for_a618(self):
        patch = (ROOT / 'platform/coachz-prelink/patches/gpu/Adreno740.patch').read_text()
        # Kernel objects above the 1 MiB GMEM window; the a6xx_submit() fence, no threads, and no hardware read-pointer shadow.
        for fact in ['+    GpuSqeIova=0x1000000', '+    RingFenceEvent = 0x04', '+    RingNoUpdate = 1u << 27', '+    RingFramedFenceWords = 7']:
            self.assertIn(fact, patch)

    def test_the_driver_only_starts_after_hw_init(self):
        seq = new_file(PATCH, 'bsd/dev/coachz_gpu_seq.h')
        self.assertIn('if (mode == 9 && !r->error && r->core_registers_done) {', seq)
        glue = new_file(PATCH, 'bsd/dev/coachz_gpu.c')
        self.assertLess(glue.index('if (r.stage == 8) {'), glue.index('coachz_gpu_ready = 1;'))
        board = (ROOT / 'platform/coachz-prelink/package/CoachzGpu.cpp').read_text()
        self.assertIn('0x06010800u, 0x80000u, 0x100000ull', board)   # a618's chip, GMEM size and base
        self.assertIn('native_adreno_publish(vm,', board)
        self.assertLess(board.index('coreWrite(CpSqeCntl, 1);'), board.index('coreWrite(RbbmSecvidTrustCntl, 0);'))

    def test_launchd_waits_and_the_plug_in_is_shown(self):
        patch = (ROOT / 'platform/coachz-prelink/patches/xnu/coachz-prelink-gpu-addon.patch').read_text()
        self.assertIn('+coachz_gpu_prepare(proc_t p, user_addr_t scratch)', patch)
        self.assertIn('native_gpu_addon_install(p, scratch);', patch)
        self.assertIn('NativeAdrenoMetal.bundle/Info.plist', patch)
        self.assertIn('coachz-prelink-gpu-addon.patch', (ROOT / 'platform/coachz-prelink/patches/xnu/series').read_text())


class CoreTests(unittest.TestCase):
    def test_register_facts(self):
        source = new_file(PATCH, 'bsd/dev/coachz_gpu_core.h')
        # a6xx.xml dword offsets, from sc7180.dtsi's GPU base, and a6xx_gmu.xml's in bytes.
        for fact in ['GPU_CORE_BASE 0x05000000u', 'CP_RB_BASE 0x0800u', 'CP_RB_WPTR 0x0807u', 'CP_SQE_CNTL 0x0808u', 'CP_SQE_INSTR_BASE 0x0830u', 'CP_PROTECT 0x0850u',
                     'RBBM_SECVID_TRUST_CNTL 0xf400u', 'GBIF_HALT 0x3c45u', 'RBBM_GBIF_HALT 0x0016u', 'GMU_DCVS_PERF_SETTING 0x8ff4u', 'GMU_CGC_MODE_CNTL 0x24c24u',
                     'CORE_RB_CNTL ((12u) | (2u << 8) | (1u << 27))', 'CORE_CLOCK_CNTL_ON 0x8aa8aa82u']:
            self.assertIn(fact, source)
        # The first and last of the tables Linux gives a618: a615_hwcg and a630_protect_regs.
        self.assertIn('{0x0b0u, 0x02222222u}', source)
        self.assertIn('{0x109u, 0x00004000u}', source)
        self.assertIn(hex((0x3fff & 0x04ff) << 18), hex(0x13fc0000))
        self.assertIn('0x13fc0000u', source)
        self.assertIn('0x80011c00u'.replace('0x80011c00', '%08x' % (1 << 31 | 0x11c00)), source)

    def test_no_64_bit_store_to_core_registers(self):
        # Many are not 8-byte aligned; a 64-bit store to UCHE_WRITE_RANGE_MAX panicked the hardware.
        source = new_file(PATCH, 'bsd/dev/coachz_gpu_core.h')
        self.assertNotIn('gpu_w64(io, GPU_CORE', source)
        self.assertNotIn('gpu_r64(io, GPU_CORE', source)

    def test_order(self):
        source = new_file(PATCH, 'bsd/dev/coachz_gpu_core.h')
        run = source[source.index('static inline void core_run('):source.index('static inline void core_stop(')]
        self.assertLess(run.index('core_map(io, r)'), run.index('core_load_sqe(io, r)'))
        self.assertLess(run.index('core_load_sqe(io, r)'), run.index('core_registers(io, r)'))
        self.assertLess(run.index('core_registers(io, r)'), run.index('core_commands(io, r)'))
        commands = source[source.index('static inline void core_commands('):source.index('static inline void core_run(')]
        self.assertLess(commands.index('CP_SQE_INSTR_BASE'), commands.index('core_w(io, CP_SQE_CNTL, 1u);'))
        # GPU_SET stays up while the GPU's registers are in use.
        gmu = new_file(PATCH, 'bsd/dev/coachz_gpu_gmu.h')
        self.assertLess(gmu.index('core_run(io, r);'), gmu.index('the OOB cleared again'))
        self.assertLess(gmu.index('if (r->own_core) core_stop(io, r);'), gmu.index('gmu_gx_shutdown(io, r);\n\t}\n\tif (r->own_cm3)'))


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
                screen.pack(b'', b'', 0, 0, prelinked=True, gpu=8, gmu_firmware=bytes(16))   # stage 8 also needs the SQE firmware
            with self.assertRaises(ValueError):
                screen.pack(b'', b'', 0, 0, prelinked=True, gpu=9, gmu_firmware=bytes(16), sqe_firmware=bytes(16))   # the driver also needs the plug-in
            root2 = screen.Node()
            root2.set('name', b'device-tree\0')
            root2.child('chosen').child('memory-map')
            adt2 = screen.prelink_tree(root2.write(), window, tree, ramdisk, cache, gmu_firmware_size=0x8000, sqe_firmware_size=0x8000 + 12)
            entries2 = {key: value for key, _, value in screen.read_tree(adt2).child('chosen').child('memory-map').props}
            gmu_base, _ = struct.unpack('<QQ', entries2['CoachzGmuFirmware'])
            sqe_base, sqe_size = struct.unpack('<QQ', entries2['CoachzSqeFirmware'])
            self.assertEqual((sqe_base, sqe_size), (gmu_base + 0x8000, 0x8000 + 12))
            with self.assertRaises(ValueError):
                screen.pack(b'', b'', 0, 0, prelinked=True, gpu=2, gmu_firmware=bytes(16))


if __name__ == '__main__':
    unittest.main()
