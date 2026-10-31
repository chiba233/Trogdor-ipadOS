# Wi-Fi implementation references

Target: native Coachz / SC7180, WCN3990 over SNOC. These are fixed source revisions, not hardware acceptance. The new code is BSD-2-Clause; the Linux files were consulted for wire/layout facts, not copied as GPL implementation into BSD files. No third-party source tree or firmware is vendored here.

## SC7180 hardware facts

- [sc7180.dtsi](https://github.com/torvalds/linux/blob/36aad21d50d1af3f6954bb0c988257a908202477/arch/arm64/boot/dts/qcom/sc7180.dtsi): SMEM at `0x80900000/0x200000`; SMP2P MPSS items 435/428, APPS/MODEM hosts 0/1; GLINK remote PID 1; WLAN `0x18800000/0x800000`, host SID `0xc0` with mask 1.
- [Trogdor DTS](https://github.com/torvalds/linux/blob/36aad21d50d1af3f6954bb0c988257a908202477/arch/arm64/boot/dts/qcom/sc7180-trogdor.dtsi): MSA `0x94100000/0x200000`, firmware SID `0xc2` with mask 1, `qcom,msa-fixed-perm`, MPSS/MBA regions and the non-LTE firmware names. These remain target inputs to check before a later active stage.
- [qcom_q6v5_mss.c](https://github.com/torvalds/linux/blob/36aad21d50d1af3f6954bb0c988257a908202477/drivers/remoteproc/qcom_q6v5_mss.c): SC7180 MSS PIL resource table and boot path. A direct copy of Pixel PAS init/mem_setup/auth_reset is not supplied.

## Portable parsers and control protocol

- [Linux smem.c](https://github.com/torvalds/linux/blob/36aad21d50d1af3f6954bb0c988257a908202477/drivers/soc/qcom/smem.c): `$TOC` header 32 bytes, entries 48 bytes, `$PRT` header 32 bytes, private item header 16 bytes, ascending uncached and descending cached entries. The reviewed blob is `b5e7bd8c1512b389ab9471b4f912612b1465f515`.
- [Linux smp2p.c](https://github.com/torvalds/linux/blob/36aad21d50d1af3f6954bb0c988257a908202477/drivers/soc/qcom/smp2p.c): `$SMP`, 20-byte header, 16-byte entry name plus 32-bit value, versions 1/2, at most 16 entries. Reviewed blob `01b8580c9f27128b1594bc87a3e5311938f0bc14`.
- [ath10k WLFW definitions](https://github.com/torvalds/linux/blob/36aad21d50d1af3f6954bb0c988257a908202477/drivers/net/wireless/ath/ath10k/qmi_wlfw_v01.h), [ath10k QMI](https://github.com/torvalds/linux/blob/36aad21d50d1af3f6954bb0c988257a908202477/drivers/net/wireless/ath/ath10k/qmi.c): control message IDs, result/capability structures, MSA negotiation and fixed-permission distinction.
- [Flame qmi.c](https://github.com/NWylynko/ios-flame/blob/745c4341af86089efb0df851fa8a27fb8face624/phone/wifi/qmi.c), [wlfw.c](https://github.com/NWylynko/ios-flame/blob/745c4341af86089efb0df851fa8a27fb8face624/phone/wifi/wlfw.c), [link.h](https://github.com/NWylynko/ios-flame/blob/745c4341af86089efb0df851fa8a27fb8face624/phone/wifi/link.h), [glink.c](https://github.com/NWylynko/ios-flame/blob/745c4341af86089efb0df851fa8a27fb8face624/phone/wifi/glink.c): same-chip iOS protocol reference, the 7-byte QMI header and WLFW TLVs, 32-byte QRTR v1 header, and GLINK descriptor/FIFO item identities. No Flame source was imported wholesale; its Pixel physical addresses, firmware selection, MAC and permission operations are not used.

## What is intentionally still absent

No APPS-owned SMEM allocation or FIFO writes, no MPSS boot/SCM/RPMh, no live QRTR endpoint, no BDF loader, no CE ring or firmware SMMU mapping, no station/WPA2 or Apple80211 implementation. The fixed protocol fixtures verify the new helpers, not these later features.

The prototype reads only metadata/control state, not firmware contents, calibration records or network traffic. A cached SMP2P ready bit or an old FIFO allocation does not prove current remote-processor liveness.
