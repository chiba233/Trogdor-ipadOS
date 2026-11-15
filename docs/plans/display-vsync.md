# Panel vsync

The display driver (NativeLegacyDisplay, `patches/display/GopFramebuffer.patch`) sent the render server's vsync from a 60 Hz work-loop
timer (`sheng-vsync`), because UniXNU's GOP scanout has no vblank interrupt. On this board the display controller has one, so the
driver now takes the panel's own vsync when it can and keeps the timer otherwise.

Mainline Linux v6.12 gives the path:

| What | Value | Source |
| --- | --- | --- |
| Interface of the DSI output (SN65DSI86 to eDP) | INTF_1 | `dpu_6_2_sc7180.h` |
| Its vsync interrupt | MDP_SSPP_TOP0_INTR bit 27 (underrun bit 26) | `dpu_6_2_sc7180.h` |
| MDP top interrupt registers | INTR_EN 0x010, INTR_STATUS 0x014, INTR_CLEAR 0x018 of the MDP at 0x0ae01000 | `dpu_hwio.h`, `dpu_intr_set_legacy`, `sc7180.dtsi` |
| MDSS wrapper | HW_INTR_STATUS 0x0010 at 0x0ae00000, bit 0 is the MDP | `msm_mdss.c`, `sc7180.dtsi` |
| GIC | SPI 83, level high | `sc7180.dtsi` |

`drivers/RamBlock/vsync/coachz_vsync.c` attaches SPI 83 through ArmCpu and handles it as `dpu_core_irq` does: read the MDP status,
clear what was read, keep only the enabled bits. Only INTF_1's vsync bit is enabled. Each vsync is stamped with `mach_absolute_time`
in the interrupt and handed to the display driver by a high-priority thread call; the driver sends it on its work loop with the
original `native_legacy_vsync_notify`, as its tick did, only while a client has vsync notifications. The scanout flip keeps polling
its own latch. Logs: `COACHZ display: refresh event 6 … panel vsync interrupt` once, then `COACHZ vsync: N interrupts, M delivered`
every 600 deliveries.

On hardware the driver logs `INTF_1 vsync on SPI 83` and `refresh event 6 … panel vsync interrupt`, and the counters run at about
611 interrupts per 600 deliveries: a vsync that arrives while the previous delivery is still queued joins it.
