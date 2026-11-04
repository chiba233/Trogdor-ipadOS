"""Host checks for the SDHCI (eMMC/microSD) driver. Never builds or boots a kernel."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
PENDING = ROOT / 'platform/coachz-prelink/drivers/RamBlock/mmc'

# Just enough of the XNU kernel interfaces for a syntax and type check of coachz_sdcc.c on the host.
STUBS = {
    'sys/param.h': '#include <stdbool.h>\n#include <stddef.h>\n#include <stdint.h>\n',
    'sys/systm.h': 'int printf(const char *, ...);\n',
    'sys/errno.h': ''.join(f'#define {name} {value}\n' for name, value in [
        ('EIO', 5), ('ENOMEM', 12), ('EFAULT', 14), ('ENODEV', 19), ('EINVAL', 22), ('EAGAIN', 35), ('ETIMEDOUT', 60),
        ('ENOSYS', 78), ('EILSEQ', 92), ('EBADMSG', 94), ('EOPNOTSUPP', 102)]),
    'kern/clock.h': '#define NSEC_PER_MSEC 1000000ull\n#define NSEC_PER_SEC 1000000000ull\n'
                    'void clock_interval_to_deadline(uint32_t, uint32_t, uint64_t *);\n'
                    'void absolutetime_to_nanoseconds(uint64_t, uint64_t *);\nuint64_t mach_absolute_time(void);\n',
    'kern/thread.h': 'typedef struct thread *thread_t;\ntypedef int wait_result_t;\ntypedef int kern_return_t;\n'
                     '#define KERN_SUCCESS 0\ntypedef void (*thread_continue_t)(void *, wait_result_t);\n'
                     'kern_return_t kernel_thread_start(thread_continue_t, void *, thread_t *);\n'
                     'void thread_deallocate(thread_t);\n',
    'kern/sched_prim.h': '#define THREAD_UNINT 0\n#define THREAD_CONTINUE_NULL ((thread_continue_t)0)\n'
                         'wait_result_t assert_wait_deadline(void *, int, uint64_t);\n'
                         'wait_result_t thread_block(thread_continue_t);\nvoid thread_wakeup(void *);\n',
    'machine/machine_routines.h': 'typedef uintptr_t vm_offset_t;\ntypedef uintptr_t vm_size_t;\n',
    'pexpert/pexpert.h': 'int PE_parse_boot_argn(const char *, void *, int);\n',
}


class MmcTests(unittest.TestCase):
    def compiler(self):
        cc = shutil.which('cc')
        self.assertIsNotNone(cc, 'a host C compiler is required for the fixtures')
        return cc

    def test_portable_model(self):
        with tempfile.TemporaryDirectory(prefix='coachz-mmc-host-') as directory:
            binary = Path(directory) / 'mmc-model'
            subprocess.run([self.compiler(), '-std=c11', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                            '-I', str(PENDING), str(ROOT / 'tests/coachz_mmc_model_test.c'), '-o', str(binary)], check=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual(result.stdout.count('PASS '), 10, result.stdout)

    def test_kernel_file_type_checks_against_stubs(self):
        with tempfile.TemporaryDirectory(prefix='coachz-mmc-stubs-') as directory:
            stubs = Path(directory)
            for name, text in STUBS.items():
                (stubs / name).parent.mkdir(parents=True, exist_ok=True)
                (stubs / name).write_text(text)
            result = subprocess.run([self.compiler(), '-std=gnu11', '-fsyntax-only', '-Wall', '-Wextra', '-Werror',
                                     '-Wno-unused-function', '-I', str(stubs), '-I', str(PENDING), str(PENDING / 'coachz_sdcc.c')],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_emmc_writes_stay_in_the_carved_window(self):
        source = (PENDING / 'coachz_sdcc.c').read_text()
        # Each window by its GPT name at exactly where it was carved; the log and Data only with the system on this disk.
        self.assertIn('{"coachz-scratch", 242171904ull, 2097152ull, 0},', source)
        self.assertIn('{"coachz-log", 126803968ull, 524288ull, 1},', source)
        self.assertIn('{"coachz-data", 127328256ull, 113795072ull, 1},', source)
        # The boot-time write and speed self-test is gone: nothing writes the card or the eMMC unasked at boot.
        self.assertNotIn('coachz_sdcc_write_selftest', source)
        write = source[source.index('int coachz_sdcc_write(int slot, uint64_t block, uint32_t count, const void *source)'):]
        check = write.index('s->writes_refused++;')
        self.assertLess(check, write.index('request(s, &cmd, &d)'))   # the window is checked before any command

    def test_hs400_follows_the_mainline_sequence(self):
        source = (ROOT / 'platform/coachz-prelink/drivers/RamBlock/mmc/coachz_sdcc.c').read_text()
        body = source[source.index('static int mmc_select_hs400es('):source.index('static int mmc_attach(')]
        # mmc_select_hs400es: the PLL, 1.8 V, DDR with strobe, HS400 timing, then the rate; only the volatile EXT_CSD bytes.
        order = [body.index(s) for s in ('gpll6_enable()', 'set_signal_voltage(s, CM_SIGNAL_180)', 'CM_EXT_CSD_DDR_BUS_WIDTH_8 | CM_EXT_CSD_BUS_WIDTH_STROBE',
                                         'CM_EXT_CSD_TIMING_HS400', 'set_clock(s, CM_MMC_HS200_MAX_DTR)')]
        self.assertEqual(order, sorted(order))
        self.assertEqual(set(re.findall(r'CM_EXT_CSD_(BUS_WIDTH|HS_TIMING|PART_CONFIG|ERASE\w*),', body)), {'BUS_WIDTH', 'HS_TIMING'})
        # msm_init_cm_dll's reset order, and the calibration only above 100 MHz.
        init = source[source.index('static int dll_init('):source.index('static int dll_calibrate(')]
        steps = [init.index(s) for s in ('| CM_MSM_DLL_RST', '| CM_MSM_DLL_PDN', '& ~CM_MSM_DLL_RST', '& ~CM_MSM_DLL_PDN', '| CM_MSM_DLL_EN', '| CM_MSM_CK_OUT_EN', 'CM_MSM_DLL_LOCK')]
        self.assertEqual(steps, sorted(steps))
        self.assertIn('s->host_clock > CM_MSM_CORE_FREQ_100MHZ && s->enhanced_strobe && !s->calibration_done', source)

    def test_in_the_build(self):
        series = (ROOT / 'platform/coachz-prelink/patches/xnu/series').read_text().split()
        self.assertIn('coachz-prelink-mmc.patch', series)
        self.assertIn('coachz_mmc_start();', (ROOT / 'platform/coachz-prelink/patches/xnu/coachz-prelink-mmc.patch').read_text())
        build = (ROOT / 'platform/coachz-prelink/drivers/RamBlock/build-usb-stack.py').read_text()
        self.assertIn("mmc / 'coachz_sdcc.c'", build)
        self.assertIn("'CoachzMmcBlock.o'", build)
        self.assertIn("' coachz-emmc=3 coachz-sd=2'", (ROOT / 'tools/xnu_screen.py').read_text())
        # The shared RPMh client of coachz_rpmh_iface.h lives in the Wi-Fi patch, with cx aggregated over its consumers.
        wifi = (ROOT / 'platform/coachz-prelink/patches/xnu/coachz-prelink-wifi.patch').read_text()
        for name in ('coachz_cmd_db_address', 'coachz_cmd_db_aux', 'coachz_rpmh_write', 'coachz_rpmh_cx_request'):
            self.assertIn('+' + name + '(', wifi, name)
        self.assertIn('coachz_cx_vote(COACHZ_CX_MSS_PROXY', wifi)
        self.assertNotIn('wifi_rpmh_send(wifi_mss_rails,', wifi)
        # Seventeen interrupt lines (touch, twelve copy engines, two cores' host and power lines) need more than 16 slots.
        self.assertIn('#define ARMCPU_SPI_SLOTS 32', (ROOT / 'platform/coachz-prelink/patches/drivers/ArmCpu.patch').read_text())

if __name__ == '__main__':
    unittest.main()
