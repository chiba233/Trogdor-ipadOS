# Sources

Mainline Linux `torvalds/linux` at `6628e7d4c6b25c8d18f1345e3a20d21829aae7e4` (Linux 7.4-rc1). Read from raw files at that commit; nothing is copied into this repository except the generated QMP register/value table (facts produced by `gen_qmp_tables.py`). Linux is GPL-2.0; the code here is written for this project.

| Linux file | What was taken |
|---|---|
| `arch/arm64/boot/dts/qcom/sc7180.dtsi` | usb_1 glue `0xa6f8800` (legacy `qcom,dwc3` binding), clocks and assigned rates, GDSC, `required-opps` nom, BCR, interrupts; usb_1_dwc3 `0xa600000`/SPI 133, quirks `dis_u2_susphy`, `dis_enblslpm`, `parkmode-disable-ss`, `dis-u1/u2-entry`, super-speed; usb_1_hsphy `0x88e3000` (cfg_ahb, ref, QUSB2 BCR, `qusb2p_hstx_trim` at 0x25b bits 1..3); usb_1_qmpphy `0x88e8000` (aux, ref, com_aux, usb3_pipe, cfg_ahb; phy and common BCRs); qfprom regions |
| `arch/arm64/boot/dts/qcom/sc7180-trogdor.dtsi` | `dr_mode = "host"`, RTS5411 hub halves on ports 1 and 2 with `vdd-supply = <&pp3300_hub>` (gpio84, boot-on), QUSB2 tuning and supplies (ldoa4, ldoa11, ldoa17), QMP supplies (ldoa4, ldoc3), initial modes, cros-ec-typec connectors |
| `drivers/usb/dwc3/dwc3-qcom-legacy.c` | glue probe order (resets, clocks), QSCRATCH registers, host mode without VBUS override |
| `drivers/usb/dwc3/core.c`, `core.h` | `dwc3_core_probe`, `dwc3_core_is_valid`, `dwc3_get_num_ports`, `dwc3_core_init`, PHY setup, `dwc3_enable_susphy`, soft reset, global control, event buffers, GUCTL1/2/3 rules, `dwc3_set_prtcap`, register offsets, bits and revision constants |
| `drivers/usb/host/xhci-ext-caps.h` | extended capability walk, protocol major revision and port count fields |
| `drivers/usb/misc/onboard_usb_dev.h` | RTS5411 (`usbbda,5411`, `usbbda,411`): one `vdd` supply, `reset_us` 0 |
| `drivers/phy/qualcomm/phy-qcom-qusb2.c`, `include/dt-bindings/phy/phy-qcom-qusb2.h` | `qusb2_v2_phy_cfg`, `qusb2_v2_init_tbl` and layout, `qusb2_phy_init`, overrides, efuse trim into TUNE1, supply names, CORE_READY |
| `drivers/phy/qualcomm/phy-qcom-qmp-combo.c`, `phy-qcom-qmp-common.h`, `phy-qcom-qmp.h`, `phy-qcom-qmp-dp-com-v3.h`, `phy-qcom-qmp-pcs-v3.h`, `phy-qcom-qmp-qserdes-{com,txrx}-v3.h` | `sc7180_usb3dpphy_cfg`, v3 offsets and layout, the four USB3 tables (generated), `qmp_combo_com_init`, `qmp_combo_usb_power_on`, clock/reset/supply lists, bits |
| `drivers/clk/qcom/gcc-sc7180.c`, `clk-rcg2.c`, `clk-branch.c`, `gdsc.c`, `gdsc.h` | USB clocks, RCGs and frequency tables, parent map 0, BCRs, `usb30_prim_gdsc`, branch halt rules (HALT_SKIP, hwcg), rcg2 rounding, GDSC init/enable |
| `drivers/regulator/qcom-rpmh-regulator.c`, regulator core behaviour | LDO types, minimum-voltage set at registration for range regulators |
| `drivers/pinctrl/qcom/pinctrl-sc7180.c`, `pinctrl-msm.c` | gpio84 on the west tile, GPIO control/io bits |
| `drivers/nvmem/qfprom.c` | reads from the corrected region (first reg) |

UniXNU's community catalogue lists NativeDwc3 for the Xiaomi Pad 5 (SM8150) with no recorded hardware result; it was not used.
