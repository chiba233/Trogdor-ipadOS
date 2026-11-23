"""Host checks for the rear camera's step-1 driver (platform/coachz-prelink/drivers/RamBlock/camera/coachz_camera.c).

They cover the source-derived values, the generated register tables, the wiring into the build and the /chosen switch. Nothing here is a
hardware result."""
import importlib.util
import json
import re
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DRIVER = ROOT / 'platform/coachz-prelink/drivers/RamBlock/camera/coachz_camera.c'
PATCHES = ROOT / 'platform/coachz-prelink/patches/xnu'


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    sys.path.insert(0, str(path.parent))
    spec.loader.exec_module(module)
    return module


def define(source, name):
    return int(re.search(r'#define ' + name + r'\s+\(?(0x[0-9a-fA-F]+|\d+)', source).group(1), 0)


class CameraTests(unittest.TestCase):
    def setUp(self):
        self.source = DRIVER.read_text()

    def test_phy_lane_enable_and_settle_count(self):
        # cam_csiphy_config_dev: data lanes take two bits each and the clock lane bit 7; for all five lanes that is the 0xd5 the
        # ChromeOS common table carries. csiphy_settle_cnt_calc: 85 ns + 6 UI in 300 MHz timer periods, less 6, at the 180 MHz link.
        mask, enable, bit, i = define(self.source, 'CSIPHY_LANE_MASK'), 0, 1, 0
        while i < 4:
            if bit == 2:
                enable |= 0x80 if mask & bit else 0
                i -= 1
            elif mask & bit:
                enable |= 1 << (i << 1)
            i += 1
            bit <<= 1
        self.assertEqual((mask, enable), (0x1f, 0xd5))
        ui = 10 ** 12 // 180000000 // 2
        self.assertEqual((85000 + 6 * ui) // (10 ** 12 // 300000000) - 6, 24)
        self.assertIn('85000ull + 6ull * ui', self.source)
        self.assertIn('ui = 1000000000000ull / cam->link_hz / 2', self.source)
        # The front OV5675: data 0, clock and data 1 (0x85 enabled), 450 MHz link, CSID lanes 0 and 1.
        self.assertIn('.lane_mask = 0x7u, .lanes = 2, .lane_cfg = 0x10u, .link_hz = OV5675_LINK_HZ', self.source)
        ui = 10 ** 12 // 450000000 // 2
        self.assertEqual((85000 + 6 * ui) // (10 ** 12 // 300000000) - 6, 21)

    def test_clock_rates_follow_camcc(self):
        # camcc-sc7180.c: PLL0 600 MHz (its even output undivided), PLL3 1080 MHz; RCG pre-divider fields are 2 * divider - 1.
        pll0, pll3, xo = 19.2e6 * (0x1f + 0x4000 / 65536), 19.2e6 * (0x38 + 0x4000 / 65536), 19.2e6
        sources = {'SRC_PLL0_EVEN': pll0, 'SRC_PLL3_MAIN': pll3, 'SRC_XO': xo}
        wanted = {'RCG_SLOW_AHB': 80e6, 'RCG_CCI0': 37.5e6, 'RCG_CPHY_RX': 270e6, 'cam->rcg_phytimer': 300e6, 'RCG_IFE0_CSID': 270e6,
                  'RCG_IFE0': 360e6, 'cam->rcg_mclk': 19.2e6, 'RCG_MCLK0': 19.2e6}
        calls = re.findall(r'rcg\((RCG_\w+|cam->rcg_\w+), (SRC_\w+), (\d+), (?:true|false), "\w+"\)', self.source)
        # The sensor descriptions behind cam->: camcc-sc7180.c csi0/csi1phytimer_clk_src 0x5004/0x5028, mclk0/1_clk_src 0x4004/0x4024.
        self.assertIn('.rcg_mclk = RCG_MCLK1, .cbcr_mclk = CBCR_MCLK1, .rcg_phytimer = RCG_CSI1PHYTIMER', self.source)
        self.assertIn('.rcg_mclk = RCG_MCLK0, .cbcr_mclk = CBCR_MCLK0, .rcg_phytimer = RCG_CSI0PHYTIMER', self.source)
        self.assertEqual((define(self.source, 'RCG_CSI0PHYTIMER'), define(self.source, 'CBCR_CSI0PHYTIMER'),
                          define(self.source, 'CBCR_CSIPHY0'), define(self.source, 'RCG_CSI1PHYTIMER')), (0x5004, 0x501c, 0x5020, 0x5028))
        self.assertEqual({name for name, _, _ in calls}, set(wanted))
        for name, source, field in calls:
            self.assertAlmostEqual(sources[source] / ((int(field) + 1) / 2), wanted[name], delta=1e3, msg=name)
        self.assertEqual((define(self.source, 'SRC_PLL3_MAIN'), define(self.source, 'SRC_PLL0_EVEN')), (5, 6))

    def test_cci_timing_is_the_v2_table(self):
        # i2c-qcom-cci.c cci_v2_data standard mode at 37.5 MHz, the same numbers sc7180-camera.dtsi gives.
        for value in ('(201u << 16) | 174u', '(204u << 16) | 231u', '(22u << 16) | 162u', '227u', '(0u << 8) | (6u << 4) | 3u'):
            self.assertIn(value, self.source)

    def test_csid_and_vfe_values(self):
        self.assertEqual(define(self.source, 'CSID_LANE_CFG'), 0x3210)
        self.assertEqual(define(self.source, 'MIPI_DT_RAW10'), 0x2b)
        self.assertEqual(define(self.source, 'WM_RDI_DEFAULT'), 0xff01)
        self.assertEqual(define(self.source, 'VFE_RESET_HW_AND_REG'), 0x3f9f)
        # Low DRAM for the two frames: past the presenter's buffer pair at 0x82400000 and before mpss_mem at 0x86000000.
        end = 0x84400000 + (1640 * 10 + 7) // 8 * 1232
        self.assertLess(0x82400000 + 2 * 2160 * 1440 * 4, 0x84000000)
        self.assertLess(end, 0x86000000)

    def test_generated_tables(self):
        tables = load('camera_tables', ROOT / 'tools/camera_tables.py')
        lock = json.loads((ROOT / 'upstream.lock.json').read_text())['sources']
        cached = [tables.CACHE / name / path for name in ('linux-camera', 'chromiumos-camx') for path in lock[name]['files']]
        if not all(path.is_file() for path in cached):
            self.skipTest('needs the pinned camera sources under upstream/camera (tools/camera_tables.py fetches them)')
        with tempfile.TemporaryDirectory() as directory:
            header = Path(directory) / 'coachz_camera_tables.h'
            subprocess.run([sys.executable, str(ROOT / 'tools/camera_tables.py'), str(header)], check=True)
            text = header.read_text()
        self.assertIn('{0x0103, 0x01},\n\t{0x0100, 0x00},', text)   # the 360 Mbps list starts with the software reset
        self.assertIn('{0x3813, 0x01}', text)                         # SGRBG10
        self.assertIn('#define OV8856_MODE_HTS 3820u', text)
        self.assertIn('#define OV8856_MODE_VTS 1256u', text)
        self.assertIn('{0x0814, 0xd5, 0, CAM_PHY_LANE_ENABLE}', text)
        self.assertIn('#define CSIPHY_2PH_SIZE 23', text)
        self.assertEqual(len(re.findall(r'CAM_PHY_SETTLE_CNT_LOWER_BYTE', text)), 5)

    def test_no_gpl_tables_tracked(self):
        self.assertIn('#include "coachz_camera_tables.h"', self.source)
        tracked = subprocess.run(['git', 'ls-files', '--', 'platform', 'tools', 'boot'], cwd=ROOT, capture_output=True, text=True).stdout
        self.assertNotIn('coachz_camera_tables.h', tracked)
        self.assertNotRegex(self.source, r'\{0x3[0-9a-f]{3}, 0x[0-9a-f]{2}\},\s*\{0x3[0-9a-f]{3}')

    def test_in_the_build(self):
        build = (ROOT / 'platform/coachz-prelink/drivers/RamBlock/build-usb-stack.py').read_text()
        self.assertIn("camera / 'coachz_camera.c'", build)
        self.assertIn("'tools/camera_tables.py'", (ROOT / 'tools/kernel_source.py').read_text())
        series = [line for line in (PATCHES / 'series').read_text().splitlines() if line and not line.startswith('#')]
        self.assertLess(series.index('coachz-prelink-camera.patch'), series.index('coachz-prelink-baseband.patch'))
        patch = (PATCHES / 'coachz-prelink-camera.patch').read_text()
        self.assertIn('+			coachz_camera_start();', patch)
        self.assertIn('COACHZ_CX_CONSUMERS = 7', patch)
        self.assertIn('{"MM0", 32, 1, 0,', patch)
        self.assertEqual(define(self.source, 'CX_CONSUMER_CAMERA'), 6)

    def test_service_and_viewer(self):
        service = (ROOT / 'platform/coachz-prelink/drivers/RamBlock/camera/CoachzCamera.cpp').read_text()
        self.assertIn('setProperty("NativeSandboxGrantProcess", "*")', service)
        self.assertIn('setProperty("IOUserClientClass", "CoachzCameraUserClient")', service)
        self.assertIn('IOMemoryDescriptor::withPhysicalAddress(coachz_camera_frame_pa(index)', service)
        self.assertIn('if ((cam_mode & 0xffu) == 2) {', self.source)
        # The ring: four 4 MiB buffers from 0x84000000, below mpss_mem at 0x86000000.
        self.assertIn('#define RING            4u', self.source)
        self.assertLess(0x84000000 + 4 * 0x400000, 0x86000000)
        build = (ROOT / 'platform/coachz-prelink/drivers/RamBlock/build-usb-stack.py').read_text()
        self.assertIn("camera / 'CoachzCamera.cpp'", build)
        self.assertIn("driver / 'CoachzCamera.o'", build)
        viewer = (ROOT / 'platform/coachz-prelink/apps/camera/CoachzCamera.m').read_text()
        self.assertIn('IOServiceMatching("CoachzCamera")', viewer)
        for selector in ('kSelStart = 0', 'kSelStop = 1', 'kSelStatus = 2', 'kSelExposure = 3', 'kSelFrame = 4'):
            self.assertIn(selector, viewer)
        apps = (ROOT / 'tools/coachz_apps.py').read_text()
        self.assertIn("('owner-apps/CoachzCamera.app/CoachzCamera', 0o755, camera_exe)", apps)
        self.assertIn('cdhashes(camera_exe)', apps)
        installer = (ROOT / 'platform/coachz-prelink/apps/coachz-apps.m').read_text()
        self.assertIn('installCameraApp()', installer)
        self.assertIn('@"com.coachz.camera"', installer)
        patch = (PATCHES / 'coachz-prelink-apps.patch').read_text()
        self.assertIn('"/var/jb/Applications/CoachzCamera.app/CoachzCamera"', patch)
        screen = (ROOT / 'tools/xnu_screen.py').read_text()
        self.assertIn("camera=(2 if a.camera_service else 1 if a.camera else 0) | (0x100 if a.camera_front_probe else 0) | (0x200 if a.camera_vcm_probe else 0)", screen)

    def test_chosen_switch(self):
        screen = load('xnu_screen', ROOT / 'tools/xnu_screen.py')
        if not screen.OS_PROFILE.is_file():
            self.skipTest('needs the pinned UniXNU OS profile')
        root = screen.Node()
        root.set('name', b'device-tree\0')
        root.child('chosen').child('memory-map')
        window, tree, ramdisk, cache = 0x80000000, 0x2000000, bytes(0x5000), bytes(0x1234)
        on = screen.read_tree(screen.prelink_tree(root.write(), window, tree, ramdisk, cache, camera=True))
        self.assertEqual(dict((k, v) for k, _, v in on.child('chosen').props)['coachz-camera'], struct.pack('<I', 1))
        off = screen.read_tree(screen.prelink_tree(root.write(), window, tree, ramdisk, cache))
        self.assertNotIn('coachz-camera', [k for k, _, _ in off.child('chosen').props])
        with self.assertRaises(ValueError):
            screen.pack(b'', b'', 0, 0, camera=True)


if __name__ == '__main__':
    unittest.main()
