# System sleep and wake

Nothing sleeps yet: the power button and the cover reach iPadOS, which turns the display off, but the SoC keeps running.

## How XNU sleeps on arm64 (21H461 source, pinned apple-xnu)

- `IOPlatformExpert::sleepKernel` calls `IOCPUSleepKernel` (iokit/Kernel/IOCPU.cpp): platform pre-sleep actions, then
  `haltCPU()` on every running non-boot `IOCPU`, then on the boot CPU, which is where the system sleeps. On wake the boot CPU
  returns there, `processor_start()` restarts every CPU left in `kIOCPUStateStopped`, then the post-resume actions run.
- `haltCPU` ends in `cpu_sleep` / `ml_arm_sleep` (osfmk/arm64/cpu.c): the boot CPU waits for every other CPU to reach
  `ARM_CPU_ON_SLEEP_PATH`, records the wake timebase, and every CPU calls `arm64_prepare_for_sleep`.
- On a non-Apple core `arm64_prepare_for_sleep` only loops on WFI: the core stays powered and a later `processor_start` would ask
  PSCI to start a core that is still on. Apple's virtual platform (`APPLEVIRTUALPLATFORM`, osfmk/arm64/machine_routines_asm.s)
  has the PSCI form: non-boot CPUs `CPU_OFF`, the boot CPU `SYSTEM_SUSPEND` with the physical address of `LowResetVectorBase`
  as entry point, so the warm boot comes back through the kernel's reset vector (osfmk/arm64/start.s).

## What this board's firmware offers

- TF-A for sc7180 (plat/qti/common/src/qti_pm.c as added in 2020, "qti/sc7180: Do shutdown handling outside qtiseclib")
  registers `pwr_domain_off`, `pwr_domain_suspend` and `get_sys_suspend_power_state`, so PSCI `CPU_OFF` and `SYSTEM_SUSPEND`
  exist; the deepest state comes from qtiseclib's idle-state table. Which exact TF-A the factory firmware carries is not
  recorded here; `PSCI_FEATURES(SYSTEM_SUSPEND)` answers that at run time without changing anything.
- Mainline Linux on trogdor suspends through PSCI with the RPMh sleep and wake sets programmed (drivers/soc/qcom/rpmh-rsc.c) and
  wakes from interrupts the PDC (drivers/irqchip/qcom-pdc.c) routes while the GIC is off; the power button and the EC's interrupt
  are such wake sources.

## Stages

1. CPU off and on: on this board `arm64_prepare_for_sleep` uses the PSCI form with SMC (`CPU_OFF` for a non-boot CPU), and
   ArmCpu supports `haltCPU` and a second `startCPU`/`initCPU` of the same core (today `initCPU(false)` panics: "secondary
   resume is unsupported"). Testable on its own by halting and restarting one A55 from the kernel, before any system sleep.
2. System suspend: the boot CPU's `SYSTEM_SUSPEND` and the warm boot through `LowResetVectorBase`: GIC distributor and
   redistributor, timer and boot CPU state restored (ArmCpu `initCPU(false)` on CPU0), the PDC wake routing for the power key
   and the EC, and the RPMh sleep set. Gated by a boot argument, because a sleep that does not wake looks like a dead device.
3. Devices: each driver's suspend and resume in the order the platform actions and IOKit power states call them (display and
   backlight, GPU/GMU, eMMC and the USB disk, Wi-Fi, modem, audio, camera, touch), following each mainline driver's PM ops.

Each stage is its own hardware acceptance; stage 2 is not offered for a build before stage 1 has passed.

## Stage 1 (passed on hardware)

On hardware with `--cpu-offline-test 150`: `PSCI version 0x10001, features CPU_SUSPEND 2 CPU_OFF 0 SYSTEM_SUSPEND 0`, so the
factory TF-A offers `SYSTEM_SUSPEND`; CPU5 went offline and came back in all three rounds (`offline test 3: processor_start 0,
cpu 5 state 3`, `cpu 5 back online`) with no panic. The boot core must have run `cpu_signal_handler` once to take IPIs
(XNU clears `SIGPdisabled` there): without the self-IPI, ArmCpu calls it from `initCPU`, or `ml_cpu_down` panics with an IPI
failure.


- `IOCPUSleepKernel` calls `haltCPU()` from the boot core. ArmCpu's old `haltCPU` disabled interrupts and waited for
  interrupts forever on the calling core, so a system sleep request would have stopped the boot core; nothing requested one so
  far. Now a non-boot core leaves through `processor_exit()` (allowed while quiescing: `ml_cpu_can_exit` in
  osfmk/arm/cpu_common.c), which runs `cpu_sleep()` on that core; without `USE_APPLEARMSMP` that ends in
  `PE_cpu_machine_quiesce` and so in ArmCpu's `quiesceCPU()` on the leaving core, which turns its timer and GIC group 1 off and
  calls `ml_arm_sleep()`. `coachz-prelink-cpu-off.patch` makes `ml_arm_sleep()` power a non-boot core off with PSCI `CPU_OFF`.
  The boot core's `haltCPU()` returns (system suspend is stage 2), so a sleep request wakes at once and restarts the others.
- `processor_start()` then calls `startCPU()` (PSCI `CPU_ON` at the reset vector), the core resumes through
  `cpu_reset_handler`, and `cpu_machine_init` calls `initCPU(false)`, which now re-enables the GIC CPU interface, the SGI
  routing, the timer and the self-IPI without registering the interrupt handler again.
- `/chosen/armcpu-offline-test` = N (`tools/xnu_screen.py --cpu-offline-test N`; the boot-argument field is full): N seconds after all cores are up, CPU5 (an A55) goes offline the way sleep takes it and comes back
  two seconds later, three times. Log lines: `COACHZ SMP: cpu 5 processor_exit 0, state ...`, `offline test k: ...`,
  `COACHZ SMP: cpu 5 back online`. ArmCpu.patch is shared with coachz-screen (a symbolic link).
- When all cores are up, ArmCpu asks the firmware (read only) for `PSCI_VERSION` and `PSCI_FEATURES` of `CPU_SUSPEND`,
  `CPU_OFF` and `SYSTEM_SUSPEND`: `COACHZ SMP: PSCI version ..., features ...`. That line decides whether stage 2 can use
  `SYSTEM_SUSPEND` on the factory TF-A.

## Stage 2 (enters and resumes on hardware, wakes at once)

On hardware with `--system-suspend-test 180`: the firmware took SYSTEM_SUSPEND and the boot core came back through our entry
(`cpu 0 processor_exit 0 after 4 ms, state 3, PSCI 1, 7 cores off`), the seven other cores came back online and the system ran
on with no panic. It woke after 4 ms: after the EC events were read the EC line read 1, so a wake source was still pending; which
one is not yet known.


Off by default; `tools/xnu_screen.py --system-suspend` sets `/chosen/coachz-system-suspend`, and `--system-suspend-test N` also
sets `/chosen/coachz-system-suspend-test` = N. With neither, the boot core's `haltCPU` returns as in stage 1.

- Down: `IOCPUSleepKernel` halts the other cores (stage 1), then the boot core's `haltCPU` reads the EC's pending events
  (`EC_CMD_GET_NEXT_EVENT` until none are left, so `EC_AP_INT_L` is released) and calls `processor_exit` on the boot processor
  (allowed while quiescing, `REASON_SYSTEM`). `cpu_sleep` ends in `quiesceCPU` on the boot core, which runs the platform's quiesce
  actions (`IOCPURunPlatformQuiesceActions`, as `IOCPUSleepKernel` expects from the boot CPU driver), saves the GIC
  (`GICD_CTLR` and each `armcpu_spi_attach` SPI's group, enable, trigger, priority and route), arms PDC pin 84 for the EC
  (level low and enabled, `qcom-pdc.c`; the TLMM needs nothing on SC7180) and sets the PSCI entry: the reset vector's physical
  address, through the EL2 stub with `armcpu-el2-entry=1`. `coachz-prelink-system-suspend.patch` then, in `ml_arm_sleep`'s boot
  branch, waits until every other core's `AFFINITY_INFO` reads OFF (as `psci_cpu_kill`) and calls SMC64 `SYSTEM_SUSPEND`.
- Refused: if `SYSTEM_SUSPEND` returns, the boot core takes the exception and interrupt stacks as `start_cpu` does and enters
  `arm_init_cpu`, the same path a wake takes, so the system comes back instead of stopping; the log shows the PSCI error.
- Up: the warm boot comes through `LowResetVectorBase`, `start_cpu` and `arm_init_cpu` to `initCPU(false)` on CPU0, which writes
  the distributor back in gic_dist_init's order (groups off, ARE, all SPIs disabled, each saved SPI, its enable, `GICD_CTLR`),
  wakes CPU0's redistributor, sets the CPU interface, SGI/PPI enables and the timer, restores the PDC pin and runs the active
  actions. `processor_exit` then returns on the boot core and `IOCPUSleepKernel` restarts the other cores (stage 1).
- Test: N seconds after all cores are up, `EC_CMD_RTC_SET_ALARM` 10 s (rtc-cros-ec.c), then `IOCPUSleepKernel` bound to the boot
  core; the RTC alarm or the power button should wake it through the EC's interrupt. Devices are not suspended (stage 3).
- Log lines: `COACHZ SLEEP: system suspend enabled`, `system suspend: EC line ...`, `cpu 0 entering SYSTEM_SUSPEND`,
  `cpu 0 resumed, PSCI ..., N cores off before it, ... ms by the counter, GIC N SPIs restored (... kept ...)`,
  `COACHZ SMP: cpu N back online`, `COACHZ SLEEP: test: awake, ... s asleep by the RTC, N of 8 secondary cores running`.
  PSCI 1 means the firmware resumed through the entry point; a negative value is its refusal.
- Not done: the RPMh sleep and wake sets (rpmh-rsc.c's `rpmh_flush` on the last core down). Without them the AOP keeps the active
  votes, so the SoC suspends without lowering the rails. The EC host sleep event (`EC_CMD_HOST_SLEEP_EVENT`, cros_ec.c) is not
  sent either. Whether the EC raises its interrupt for an RTC alarm while the AP is in SYSTEM_SUSPEND without it is not known;
  if no alarm wakes the board, the power button is the wake source. The GIC SPI of the EC's PDC pin (564) stays disabled.

## Stage 2 notes (source reading)

- The boot core wakes through `LowResetVectorBase`: start.s finds its `cpu_data` by MPIDR and jumps to `cpu_reset_handler`,
  which `cpu_sleep()` set to `start_cpu`, so it re-enters `arm_init_cpu` (osfmk/arm/arm_init.c): `SleepState` cleared, TLB
  flushed, timebase re-initialised, and on the boot core `serial_init()` and `PE_init_platform(TRUE)` again, then
  `cpu_machine_init()` calls `PE_cpu_machine_init(boot = false)`, i.e. ArmCpu `initCPU(false)` on CPU0. That branch panics today
  ("secondary/resume CPU startup is unsupported"); stage 2 has to rebuild there whatever the suspend lost: the GIC distributor
  (enables, routing, configuration and priorities of every SPI attached through `armcpu_spi_attach`), CPU0's redistributor and
  CPU interface, the SGI routing and the timer. Whether the distributor keeps its state across SYSTEM_SUSPEND on this SoC is not
  known from public source (qtiseclib is closed); mainline's gic-v3 has no distributor save/restore for this platform.
- After `initCPU(false)` returns, `IOCPUSleepKernel` restarts the other cores with `processor_start()`, which stage 1 covers.
- Why nothing ever slept: powerd's unified log (k221/k222 Data volume) prints `Sleep timer 2147483647 display timer 2` (or 10):
  the system sleep timer is "never", so no timer-driven idle sleep happens, and `IOCPUSleepKernel` was never reached
  (the old `haltCPU` would have stopped the boot core). Stage 1's code therefore runs only through the offline test. The
  "never" is iOS's default (below); whether sleep then follows the display turning off and the idle-sleep assertions
  (SpringBoard's `FBSystemApp-PreventIdleSleep` among them) being released is not verified. powerd's keys (`EnableIdleSleep`, `SleepDisabled`) are CFStrings behind chained fixups,
  so a static trace needs a fixup-aware disassembler. `CONFIG_SLEEP` is on in this kernel.
- Where the line comes from (21H461 powerd, static reading): the function at 0x10004bc50 takes the PM preferences dictionary,
  picks the power-source profile (`AC Power` when none is given), reads `System Sleep Timer` and `Display Sleep Timer` as
  numbers (-1 when absent) and prints the line only when either differs from its cached copy. 2147483647 is iOS's own default:
  IOKitUser's pwr_mgt.subproj/IOPMEnergyPrefs.c sets `kIOPMSystemSleepKey` to 2147483647 for every profile under
  `TARGET_OS_IPHONE` (10 minutes on Intel Macs, 1 on Apple silicon Macs). So the timer is not a fault of this port and nothing
  needs to change it; on iOS idle sleep is not timer driven. What triggers it instead (display off, the idle-sleep assertions
  released) is in the kernel, below.
- Kernel side (iokit/Kernel/IOPMrootDomain.cpp, `kStimulusAggressivenessChanged`): on a non-macOS target a sleep slider of
  0x7fffffff means idle sleep is enabled with `idleMilliSeconds` (the root domain's `System Idle Seconds` / idle milliseconds
  property, set by powerd) as the timer, because there is no display wrangler. When that timer expires the root domain sleeps
  unless `checkSystemSleepAllowed` refuses: `SleepDisabled`, user-disabled sleep, boot not complete, a child's
  prevent-system-sleep clamp, a driver CPU assertion, DriverKit matching or PM not ready, or the platform's
  `IOPMSetSleepSupported` answer; idle-sleep preventers (powerd's assertions) hold it off as well. So on this port idle sleep
  is armed, and either a preventer was always held or the timer never fired; before stage 1, such a sleep would have stopped
  the boot core in the old `haltCPU`, and the system has never hung. With stage 1 in the image, a sleep that does happen
  sends every driver and app through sleep and wakes at once; its marker is `COACHZ SMP: system suspend not implemented`.
- What held it off (powerd's unified log on the persistent Data, 2026-11-02 to 11-05, about 15900 assertion lines): every
  line but five reports the system state `SysAct` (a SystemIsActive assertion held), and about four in five also `PrevIdle`
  (PreventUserIdleSystemSleep). runningboardd creates and releases SystemIsActive assertions for running services almost
  continuously (6950 created, mostly CFNetwork storage of akd, mobileassetd, amsaccountsd, nsurlsessiond, familycircled);
  PreventUserIdleSystemSleep comes from audiomxd's aggregate device, dasd activities and UserEventAgent's SepUpdateTimer.
  backboardd's `com.apple.backboardd.enable-idle-sleep` (EnableIdleSleep, held while the display is off) appears six times
  for up to 11 minutes, and `SysAct` stayed on throughout each. So the idle timer never had a window without a preventer;
  whether a real iPad sees the same rate of service assertions is not known. The ~7000 `Sleep revert state: 1` lines are
  printed by the assertion path when a SystemIsActive assertion is created, not by a sleep attempt.
- powerd also raises "Powerd - prevent sleep while in usb device mode" when the USB port is in device mode, unless the boot
  argument `enable-acsleep` is set ("Boot arg enable-acsleep is set. Not preventing idle sleep for usb device mode"); the
  boot-argument field is full, and this board is a USB host, so the condition probably never holds here. That is not verified.
- Wake source for stage 2: the EC interrupt (sc7180-trogdor.dtsi `cros_ec`: GPIO94, level low, `wakeup-source`) reaches the
  PDC as pin 84 (pinctrl-sc7180.c `sc7180_pdc_map`), GIC SPI 564 by `qcom,pdc-ranges = <0 480 94>`; qcom-pdc.c programs
  `IRQ_i_CFG + 4 * pin` (level low = 0b000) and enables the pin in `IRQ_ENABLE_BANK` (before PDC 3.2) or bit 3 of the CFG
  register. The power button reaches the AP through the EC.
