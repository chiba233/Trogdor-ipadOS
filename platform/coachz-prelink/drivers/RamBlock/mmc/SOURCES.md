# Sources

Mainline Linux `torvalds/linux` at `6628e7d4c6b25c8d18f1345e3a20d21829aae7e4` (Linux 7.4-rc1, committed 2026-11-01). Read from raw files at that commit; nothing is copied into this repository. Linux is GPL-2.0: these files take its register values, tables and sequences as facts, and the code is written for this project.

| Linux file | What was taken |
|---|---|
| `arch/arm64/boot/dts/qcom/sc7180.dtsi` | sdhc_1 `0x7c4000` (+cqhci `0x7c5000`), SPI 641/644, SID 0x60, bus-width 8, non-removable, DDR/HS200/HS400/ES caps, OPP table (100 MHz low SVS, 384 MHz nom); sdhc_2 `0x8804000`, SPI 204/222, SID 0x80, bus-width 4, OPP (100 MHz low SVS, 202 MHz nom); TLMM tiles west/north/south `0x3500000/0x3900000/0x3d00000`; `rpmhpd_opp_*` levels |
| `arch/arm64/boot/dts/qcom/sc7180-trogdor.dtsi` | sdhc_1 vmmc `mcp_vcc` = ldoa19 2.96 V, vqmmc `mcp_vccq` = ldoa12 1.8 V; sdhc_2 vmmc ldoc9 2.952 V, vqmmc ldoc6 1.8–2.952 V, `cd-gpios = <&tlmm 69 GPIO_ACTIVE_LOW>`; HPM initial modes; `sdc1_on/off`, `sdc2_on/off` pin states |
| `arch/arm64/boot/dts/qcom/sc7180-trogdor-coachz.dtsi` | sdhc_2 enabled on Coachz |
| `drivers/mmc/host/sdhci-msm.c` | v5 offsets, VENDOR_SPEC POR/bits, PWRCTL bits and requests, probe order, `sdhci_msm_handle_pwr_irq`, `check_power_status`, write hooks, `set_clock` (no internal divider, `hc_select_default`), `set_uhs_signaling` (DLL reset below 100 MHz), regulator caps, signal voltage switch, quirks (BROKEN_CARD_DETECTION, SINGLE_POWER_WRITE, CAP_CLOCK_BASE_BROKEN, MULTIBLOCK_READ_ACMD12, PRESET_VALUE_BROKEN), `max_timeout_count`, `MMC_CAP_WAIT_WHILE_BUSY | MMC_CAP_NEED_RSP_BUSY`, sc7180 → sdm845 variant |
| `drivers/mmc/host/sdhci.c`, `sdhci.h` | register map and bits, reset, default interrupts, ADMA2 table and 12-byte descriptors, DMA select, transfer mode and Auto-CMD selection, command flags, 136-bit response shift, `enable_clk`, `set_bus_width`, `set_power_noreg`, `set_ios` order, timeout count, cmd/data interrupt handling, `setup_host` flags (ADMA, 64-bit, Auto-CMD23, SIGNALING_330/180, OCR), `get_vdd_value` |
| `drivers/mmc/core/core.c`, `regulator.c`, `mmc_ops.c`, `mmc_ops.h`, `mmc.c`, `sd.c`, `sd_ops.c`, `block.c` | power up/off, initial state and signal voltage, `select_voltage`, OCR bit conversions, `set_ocr`/`set_vqmmc`, `rescan_try_freq` order, CMD0/CMD1 busy polling, CID/CSD/EXT_CSD/SCR decoding (`unstuff_bits`, `tran_exp`/`tran_mant`), `init_card` order, `select_timing`/`select_hs`/`select_bus_width`/`compare_ext_csds`, `__mmc_switch` with switch status, SD `get_cid`/`init_card`/`setup_card`/`read_switch`/`switch_hs`/bus width, app commands, `CMD_ERRORS`, `R1_STATUS` |
| `include/linux/mmc/{mmc,sd,card,host,core}.h` | opcodes, response types, R1 bits, EXT_CSD indices, OCR bits, timing/width constants |
| `drivers/clk/qcom/gcc-sc7180.c`, `clk-rcg2.c`, `clk-branch.c` | SDCC1/2 AHB/APPS CBCRs, RCG offsets, frequency tables, parent maps 1 and 5, floor ops, MND/CFG programming, UPDATE poll, branch halt check |
| `drivers/regulator/qcom-rpmh-regulator.c`, `include/dt-bindings/regulator/qcom,rpmh-regulator.h` | VRM register offsets, PMIC5 LDO HPM value, pm6150/pm6150l LDO types and linear ranges, cached-voltage-until-enable behaviour, acknowledgement rules |
| `drivers/pmdomain/qcom/rpmhpd.c`, `include/dt-bindings/power/qcom,rpmhpd.h` | cmd-db ARC level mapping, corner selection, level values |
| `drivers/pinctrl/qcom/pinctrl-sc7180.c`, `pinctrl-msm.c` | SDC1/SDC2 HDRV_PULL_CTL offsets and pull/drive bits, GPIO69 on the west tile, GPIO pull/drive bits, pull and drive-strength encodings |

Cross-checks from this repository's pinned firmware sources (not used as the design source): coreboot `src/mainboard/google/trogdor/mainboard.c` writes `0x9fff` and `0x1fe4` to the SDC1/SDC2 pin registers, which is exactly what `sdc1_on`/`sdc2_on` encode (checked in the host test); depthcharge `src/board/trogdor/board.c` uses the same two host bases.

Public card data in the host test: the Samsung DX68MB CID/CSD and sector count recorded on UniXNU's gta4l device page (`upstream/unixnu-gpu/docs/zh/devices/gta4l-gta4lunix.md`).
