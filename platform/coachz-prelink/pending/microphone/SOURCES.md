# 麦克风驱动源码证据

本次实现读取的 Linux 固定版本：`36aad21d50d1af3f6954bb0c988257a908202477`（torvalds/linux）。下面保留源码片段、原始行号、blob SHA、直接链接和批注，硬件判断不依赖手机平台类推。

驱动实现为独立 BSD-2-Clause-Patent 表达；下面的 Linux 摘录仍属于各源文件原有许可证和作者，不是将 GPL 实现重新标注为 BSD。摘录仅作说明，不参加编译。完整文件的 SPDX/版权说明可从每项固定链接查看。

## 对应关系

| 实现项 | 源码依据及推导 |
| --- | --- |
| ADAU7002、供电、80 ms | Coachz DTS + adau7002 DAPM POST_PMU；不是可编程 I2C codec |
| Primary MI2S / SD0 | Trogdor MultiMedia0 CPU link 与 capture-sd-lines=0，Coachz 替换 codec |
| 双声道 S32_LE | sc7180_adau7002_snd_startup 强制 S32；codec 明确双声道、sig_bits=20 |
| 48 kHz | 本驱动选用的受支持采样率，不是声卡唯一支持率 |
| BCLK 3.072 MHz | 48000 × 32 × 2；RCG 频表 M=1,N=4,HID=19,parent=5 |
| DMA 48 KiB/两个 periods | lpass-platform.c 的非 CDC PCM buffer；6144 frames，3072 frames/period |
| WRDMA5 寄存器 | WRDMA_BASE + (channel - channel_start) × stride，故为 0x62f18000，不能再加 5 个 stride |
| WRDMA CTL 0x22100e | burst=1@21, WPSCNT_TWO=1@17, interface=1@12, watermark=7@1；bit0 为 enable |
| Primary CTL | SD0 mode=1@4、stereo=0@3、internal WS=0@2、S32=2@0；MICEN bit9；禁 loopback |
| IRQ | host port0，SPI160，global channel5 的 PER/XRUN/ERR 位15/16/17；STAT +4，CLEAR +0xc |
| 引脚 | WEST tile；53–56 mi2s_0，57 lpass_ext，均 mux1；83 GPIO mux0/pull-up/8mA |
| GPIO83 | DTS 未要求 output-high，故保留 OE，只设置 mux/pull/drive，不发明一次拉高操作 |
| 电源与 SMMU | DTS 的 rail、clock、power-domain 和 IOMMU 都是平台依赖；当前代码不会伪造这部分已完成 |

64 ms worker 截止、共享 mono ring、输入 epoch、HAL seqlock、故障隔离是本实现的策略，不冒充 Linux 原有机制。Linux 用 ALSA period/trigger 和 DMA API；这里用现有 XNU timer 与非缓存映射表达生命周期。

## Linux 证据摘录

### 1. 板级 codec、电源与 DMIC enable

[arch/arm64/boot/dts/qcom/sc7180-trogdor-coachz.dtsi](https://github.com/torvalds/linux/blob/36aad21d50d1af3f6954bb0c988257a908202477/arch/arm64/boot/dts/qcom/sc7180-trogdor-coachz.dtsi)  
Blob: `25b17b0425f24e9f4e8c94db300183cf3585b385`  
原文件许可标识：`// SPDX-License-Identifier: (GPL-2.0+ OR MIT)`

批注：这是 Coachz 的覆盖，不能使用父板 Realtek codec 的启动逻辑。80 ms 来自板级属性；GPIO83 的 pull-up 并不是 output-high。

[原始 L19–L24](https://github.com/torvalds/linux/blob/36aad21d50d1af3f6954bb0c988257a908202477/arch/arm64/boot/dts/qcom/sc7180-trogdor-coachz.dtsi#L19-L24)

```text
19: 	adau7002: audio-codec-1 {
20: 		compatible = "adi,adau7002";
21: 		IOVDD-supply = <&pp1800_l15a>;
22: 		wakeup-delay-ms = <80>;
23: 		#sound-dai-cells = <0>;
24: 	};
```

[原始 L164–L176](https://github.com/torvalds/linux/blob/36aad21d50d1af3f6954bb0c988257a908202477/arch/arm64/boot/dts/qcom/sc7180-trogdor-coachz.dtsi#L164-L176)

```text
164: 
165: &sound {
166: 	compatible = "google,sc7180-coachz";
167: 	model = "sc7180-adau7002-max98357a";
168: 	audio-routing = "PDM_DAT", "DMIC";
169: 
170: 	pinctrl-names = "default";
171: 	pinctrl-0 = <&dmic_clk_en>;
172: };
173: 
174: &sound_multimedia0_codec {
175: 	sound-dai = <&adau7002>;
176: };
```

[原始 L319–L324](https://github.com/torvalds/linux/blob/36aad21d50d1af3f6954bb0c988257a908202477/arch/arm64/boot/dts/qcom/sc7180-trogdor-coachz.dtsi#L319-L324)

```text
319: 	dmic_clk_en: dmic-clk-en-state {
320: 		pins = "gpio83";
321: 		function = "gpio";
322: 		drive-strength = <8>;
323: 		bias-pull-up;
324: 	};
```

### 2. CPU link、SD0、1800 mV 与 pins

[arch/arm64/boot/dts/qcom/sc7180-trogdor.dtsi](https://github.com/torvalds/linux/blob/36aad21d50d1af3f6954bb0c988257a908202477/arch/arm64/boot/dts/qcom/sc7180-trogdor.dtsi)  
Blob: `0e0bc8543f714e2e471110576ee8dd9a1babdfd3`  
原文件许可标识：`// SPDX-License-Identifier: (GPL-2.0+ OR MIT)`

批注：父板声明 Primary MI2S，Coachz 只替换 codec。capture-sd-lines=<0> 指 SD0；drive-strength=2 编码为0。MCLK pin 也在默认 pinctrl 中，不能只接 BCLK/WS。

[原始 L387–L397](https://github.com/torvalds/linux/blob/36aad21d50d1af3f6954bb0c988257a908202477/arch/arm64/boot/dts/qcom/sc7180-trogdor.dtsi#L387-L397)

```text
387: 		dai-link@0 {
388: 			link-name = "MultiMedia0";
389: 			reg = <MI2S_PRIMARY>;
390: 			cpu {
391: 				sound-dai = <&lpass_cpu MI2S_PRIMARY>;
392: 			};
393: 
394: 			sound_multimedia0_codec: codec {
395: 				sound-dai = <&alc5682 0 /* aif1 */>;
396: 			};
397: 		};
```

[原始 L532–L537](https://github.com/torvalds/linux/blob/36aad21d50d1af3f6954bb0c988257a908202477/arch/arm64/boot/dts/qcom/sc7180-trogdor.dtsi#L532-L537)

```text
532: 		pp1800_l15a: ldo15 {
533: 			regulator-min-microvolt = <1800000>;
534: 			regulator-max-microvolt = <1800000>;
535: 			regulator-initial-mode = <RPMH_REGULATOR_MODE_HPM>;
536: 		};
537: 
```

[原始 L760–L779](https://github.com/torvalds/linux/blob/36aad21d50d1af3f6954bb0c988257a908202477/arch/arm64/boot/dts/qcom/sc7180-trogdor.dtsi#L760-L779)

```text
760: &lpass_cpu {
761: 	status = "okay";
762: 
763: 	pinctrl-names = "default";
764: 	pinctrl-0 = <&sec_mi2s_active>, <&pri_mi2s_active>, <&pri_mi2s_mclk_active>;
765: 
766: 	#address-cells = <1>;
767: 	#size-cells = <0>;
768: 
769: 	dai-link@0 {
770: 		reg = <MI2S_PRIMARY>;
771: 		qcom,playback-sd-lines = <1>;
772: 		qcom,capture-sd-lines = <0>;
773: 	};
774: 
775: 	secondary_mi2s: dai-link@1 {
776: 		reg = <MI2S_SECONDARY>;
777: 		qcom,playback-sd-lines = <0>;
778: 	};
779: 
```

[原始 L1030–L1038](https://github.com/torvalds/linux/blob/36aad21d50d1af3f6954bb0c988257a908202477/arch/arm64/boot/dts/qcom/sc7180-trogdor.dtsi#L1030-L1038)

```text
1030: &pri_mi2s_active {
1031: 	drive-strength = <2>;
1032: 	bias-pull-down;
1033: };
1034: 
1035: &pri_mi2s_mclk_active {
1036: 	drive-strength = <2>;
1037: 	bias-pull-down;
1038: };
```

### 3. MMIO、IRQ、IOMMU 和共享 clocks

[arch/arm64/boot/dts/qcom/sc7180.dtsi](https://github.com/torvalds/linux/blob/36aad21d50d1af3f6954bb0c988257a908202477/arch/arm64/boot/dts/qcom/sc7180.dtsi)  
Blob: `70fbfb31d4a4dd0d0dd75feb37f686933ff38a14`  
原文件许可标识：`// SPDX-License-Identifier: BSD-3-Clause`

批注：地址来自 SC7180 DTS。LPASS 区间不是从其他高通 SoC 推算。采集 SMMU 流必须由平台确认，不能照搬扬声器 DMA 的成功结论。

[原始 L1675–L1680](https://github.com/torvalds/linux/blob/36aad21d50d1af3f6954bb0c988257a908202477/arch/arm64/boot/dts/qcom/sc7180.dtsi#L1675-L1680)

```text
1675: 		tlmm: pinctrl@3500000 {
1676: 			compatible = "qcom,sc7180-pinctrl";
1677: 			reg = <0 0x03500000 0 0x300000>,
1678: 			      <0 0x03900000 0 0x300000>,
1679: 			      <0 0x03d00000 0 0x300000>;
1680: 			reg-names = "west", "north", "south";
```

[原始 L2118–L2133](https://github.com/torvalds/linux/blob/36aad21d50d1af3f6954bb0c988257a908202477/arch/arm64/boot/dts/qcom/sc7180.dtsi#L2118-L2133)

```text
2118: 			sec_mi2s_active: sec-mi2s-active-state {
2119: 				pins = "gpio49", "gpio50", "gpio51";
2120: 				function = "mi2s_1";
2121: 			};
2122: 
2123: 			pri_mi2s_active: pri-mi2s-active-state {
2124: 				pins = "gpio53", "gpio54", "gpio55", "gpio56";
2125: 				function = "mi2s_0";
2126: 			};
2127: 
2128: 			pri_mi2s_mclk_active: pri-mi2s-mclk-active-state {
2129: 				pins = "gpio57";
2130: 				function = "lpass_ext";
2131: 			};
2132: 
2133: 			ter_mi2s_active: ter-mi2s-active-state {
```

[原始 L4043–L4055](https://github.com/torvalds/linux/blob/36aad21d50d1af3f6954bb0c988257a908202477/arch/arm64/boot/dts/qcom/sc7180.dtsi#L4043-L4055)

```text
4043: 		lpasscc: clock-controller@62d00000 {
4044: 			compatible = "qcom,sc7180-lpasscorecc";
4045: 			reg = <0 0x62d00000 0 0x50000>,
4046: 			      <0 0x62780000 0 0x30000>;
4047: 			reg-names = "lpass_core_cc", "lpass_audio_cc";
4048: 			clocks = <&gcc GCC_LPASS_CFG_NOC_SWAY_CLK>,
4049: 				 <&rpmhcc RPMH_CXO_CLK>;
4050: 			clock-names = "iface", "bi_tcxo";
4051: 			power-domains = <&lpass_hm LPASS_CORE_HM_GDSCR>;
4052: 			#clock-cells = <1>;
4053: 			#power-domain-cells = <1>;
4054: 
4055: 			status = "reserved"; /* Controlled by ADSP */
```

[原始 L4060–L4093](https://github.com/torvalds/linux/blob/36aad21d50d1af3f6954bb0c988257a908202477/arch/arm64/boot/dts/qcom/sc7180.dtsi#L4060-L4093)

```text
4060: 
4061: 			reg = <0 0x62d87000 0 0x68000>, <0 0x62f00000 0 0x29000>;
4062: 			reg-names = "lpass-hdmiif", "lpass-lpaif";
4063: 
4064: 			iommus = <&apps_smmu 0x1020 0>,
4065: 				<&apps_smmu 0x1021 0>,
4066: 				<&apps_smmu 0x1032 0>;
4067: 
4068: 			power-domains = <&lpass_hm LPASS_CORE_HM_GDSCR>;
4069: 			required-opps = <&rpmhpd_opp_nom>;
4070: 
4071: 			status = "disabled";
4072: 
4073: 			clocks = <&gcc GCC_LPASS_CFG_NOC_SWAY_CLK>,
4074: 				 <&lpasscc LPASS_AUDIO_CORE_CORE_CLK>,
4075: 				 <&lpasscc LPASS_AUDIO_CORE_EXT_MCLK0_CLK>,
4076: 				 <&lpasscc LPASS_AUDIO_CORE_SYSNOC_MPORT_CORE_CLK>,
4077: 				 <&lpasscc LPASS_AUDIO_CORE_LPAIF_PRI_IBIT_CLK>,
4078: 				 <&lpasscc LPASS_AUDIO_CORE_LPAIF_SEC_IBIT_CLK>;
4079: 
4080: 			clock-names = "pcnoc-sway-clk", "audio-core",
4081: 					"mclk0", "pcnoc-mport-clk",
4082: 					"mi2s-bit-clk0", "mi2s-bit-clk1";
4083: 
4084: 
4085: 			#sound-dai-cells = <1>;
4086: 			#address-cells = <1>;
4087: 			#size-cells = <0>;
4088: 
4089: 			interrupts = <GIC_SPI 160 IRQ_TYPE_LEVEL_HIGH>,
4090: 					<GIC_SPI 268 IRQ_TYPE_LEVEL_HIGH>;
4091: 			interrupt-names = "lpass-irq-lpaif", "lpass-irq-hdmi";
4092: 		};
4093: 
```

### 4. 真正决定格式的是 machine driver

[sound/soc/qcom/sc7180.c](https://github.com/torvalds/linux/blob/36aad21d50d1af3f6954bb0c988257a908202477/sound/soc/qcom/sc7180.c)  
Blob: `bc5a18883479317324994918f2a29f8b9b10b49a`  
原文件许可标识：`// SPDX-License-Identifier: GPL-2.0-only`

批注：即使 CPU DAI 和 codec 广告了 S16，Coachz 的 ADAU7002 startup 仍强制 S32_LE。实现用双声道32位 slots，之后在 CPU 端选择一路转成单声道 S16。

[原始 L392–L419](https://github.com/torvalds/linux/blob/36aad21d50d1af3f6954bb0c988257a908202477/sound/soc/qcom/sc7180.c#L392-L419)

```text
392: static int sc7180_adau7002_snd_startup(struct snd_pcm_substream *substream)
393: {
394: 	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
395: 	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);
396: 	struct snd_soc_dai *codec_dai = snd_soc_rtd_to_codec(rtd, 0);
397: 	struct snd_pcm_runtime *runtime = substream->runtime;
398: 
399: 	switch (cpu_dai->id) {
400: 	case MI2S_PRIMARY:
401: 		snd_soc_dai_set_fmt(codec_dai,
402: 				    SND_SOC_DAIFMT_CBC_CFC |
403: 				    SND_SOC_DAIFMT_NB_NF |
404: 				    SND_SOC_DAIFMT_I2S);
405: 		runtime->hw.formats = SNDRV_PCM_FMTBIT_S32_LE;
406: 		snd_pcm_hw_constraint_msbits(runtime, 0, 32, 32);
407: 
408: 		break;
409: 	case MI2S_SECONDARY:
410: 		break;
411: 	case LPASS_DP_RX:
412: 		break;
413: 	default:
414: 		dev_err(rtd->dev, "%s: invalid dai id 0x%x\n", __func__,
415: 			cpu_dai->id);
416: 		return -EINVAL;
417: 	}
418: 	return 0;
419: }
```

[原始 L443–L449](https://github.com/torvalds/linux/blob/36aad21d50d1af3f6954bb0c988257a908202477/sound/soc/qcom/sc7180.c#L443-L449)

```text
443: };
444: 
445: static const struct snd_soc_ops sc7180_adau7002_ops = {
446: 	.startup = sc7180_adau7002_snd_startup,
447: };
448: 
449: static const struct snd_soc_dapm_widget sc7180_snd_widgets[] = {
```

### 5. codec 电源和唤醒事件

[sound/soc/codecs/adau7002.c](https://github.com/torvalds/linux/blob/36aad21d50d1af3f6954bb0c988257a908202477/sound/soc/codecs/adau7002.c)  
Blob: `c9134e1de0b25b1d8a41b36156d926859d578e38`  
原文件许可标识：`// SPDX-License-Identifier: GPL-2.0-only`

批注：采集依赖 IOVDD，POST_PMU 等待指定的 wakeup-delay。20 significant bits 与32位传输容器是两个概念；实际 PCM 的对齐和信号仍需真机验证。

[原始 L22–L38](https://github.com/torvalds/linux/blob/36aad21d50d1af3f6954bb0c988257a908202477/sound/soc/codecs/adau7002.c#L22-L38)

```text
22: static int adau7002_aif_event(struct snd_soc_dapm_widget *w,
23: 			      struct snd_kcontrol *kcontrol, int event)
24: {
25: 	struct snd_soc_component *component =
26: 			snd_soc_dapm_to_component(w->dapm);
27: 	struct adau7002_priv *adau7002 =
28: 			snd_soc_component_get_drvdata(component);
29: 
30: 	switch (event) {
31: 	case SND_SOC_DAPM_POST_PMU:
32: 		if (adau7002->wakeup_delay)
33: 			msleep(adau7002->wakeup_delay);
34: 		break;
35: 	}
36: 
37: 	return 0;
38: }
```

[原始 L57–L89](https://github.com/torvalds/linux/blob/36aad21d50d1af3f6954bb0c988257a908202477/sound/soc/codecs/adau7002.c#L57-L89)

```text
57: static const struct snd_soc_dapm_widget adau7002_widgets[] = {
58: 	SND_SOC_DAPM_AIF_OUT_E("ADAU AIF", "Capture", 0,
59: 			       SND_SOC_NOPM, 0, 0, adau7002_aif_event,
60: 			       SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_POST_PMD),
61: 	SND_SOC_DAPM_INPUT("PDM_DAT"),
62: 	SND_SOC_DAPM_REGULATOR_SUPPLY("IOVDD", 0, 0),
63: };
64: 
65: static const struct snd_soc_dapm_route adau7002_routes[] = {
66: 	{ "ADAU AIF", NULL, "PDM_DAT"},
67: 	{ "Capture", NULL, "PDM_DAT" },
68: 	{ "Capture", NULL, "IOVDD" },
69: };
70: 
71: static struct snd_soc_dai_driver adau7002_dai = {
72: 	.name = "adau7002-hifi",
73: 	.capture = {
74: 		.stream_name = "Capture",
75: 		.channels_min = 2,
76: 		.channels_max = 2,
77: 		.rates = SNDRV_PCM_RATE_8000_96000,
78: 		.formats = SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S18_3LE |
79: 			SNDRV_PCM_FMTBIT_S20_3LE | SNDRV_PCM_FMTBIT_S24_LE |
80: 			SNDRV_PCM_FMTBIT_S24_3LE | SNDRV_PCM_FMTBIT_S32_LE,
81: 		.sig_bits = 20,
82: 	},
83: };
84: 
85: static const struct snd_soc_component_driver adau7002_component_driver = {
86: 	.probe			= adau7002_component_probe,
87: 	.dapm_widgets		= adau7002_widgets,
88: 	.num_dapm_widgets	= ARRAY_SIZE(adau7002_widgets),
89: 	.dapm_routes		= adau7002_routes,
```

### 6. SC7180 寄存器字段和 DMA 起点

[sound/soc/qcom/lpass-sc7180.c](https://github.com/torvalds/linux/blob/36aad21d50d1af3f6954bb0c988257a908202477/sound/soc/qcom/lpass-sc7180.c)  
Blob: `7c6a9b0fda891b2b79661cf1adb3c897bd391918`  
原文件许可标识：`// SPDX-License-Identifier: GPL-2.0-only`

批注：WRDMA 的 burst/WPSCNT 位比 RDMA 高一位，不可复制播放 CTL。Primary 的 OSR 是 mclk0，这个 pending 后端要求现有平台持续持有它。

[原始 L175–L223](https://github.com/torvalds/linux/blob/36aad21d50d1af3f6954bb0c988257a908202477/sound/soc/qcom/lpass-sc7180.c#L175-L223)

```text
175: 	return 0;
176: }
177: 
178: static const struct dev_pm_ops sc7180_lpass_pm_ops = {
179: 	SYSTEM_SLEEP_PM_OPS(sc7180_lpass_dev_suspend, sc7180_lpass_dev_resume)
180: };
181: 
182: static const struct lpass_variant sc7180_data = {
183: 	.i2sctrl_reg_base	= 0x1000,
184: 	.i2sctrl_reg_stride	= 0x1000,
185: 	.i2s_ports		= 3,
186: 	.irq_reg_base		= 0x9000,
187: 	.irq_reg_stride		= 0x1000,
188: 	.irq_ports		= 3,
189: 	.rdma_reg_base		= 0xC000,
190: 	.rdma_reg_stride	= 0x1000,
191: 	.rdma_channels		= 5,
192: 	.hdmi_rdma_reg_base		= 0x64000,
193: 	.hdmi_rdma_reg_stride	= 0x1000,
194: 	.hdmi_rdma_channels		= 4,
195: 	.dmactl_audif_start	= 1,
196: 	.wrdma_reg_base		= 0x18000,
197: 	.wrdma_reg_stride	= 0x1000,
198: 	.wrdma_channel_start	= 5,
199: 	.wrdma_channels		= 4,
200: 
201: 	.loopback		= REG_FIELD_ID(0x1000, 17, 17, 3, 0x1000),
202: 	.spken			= REG_FIELD_ID(0x1000, 16, 16, 3, 0x1000),
203: 	.spkmode		= REG_FIELD_ID(0x1000, 11, 15, 3, 0x1000),
204: 	.spkmono		= REG_FIELD_ID(0x1000, 10, 10, 3, 0x1000),
205: 	.micen			= REG_FIELD_ID(0x1000, 9, 9, 3, 0x1000),
206: 	.micmode		= REG_FIELD_ID(0x1000, 4, 8, 3, 0x1000),
207: 	.micmono		= REG_FIELD_ID(0x1000, 3, 3, 3, 0x1000),
208: 	.wssrc			= REG_FIELD_ID(0x1000, 2, 2, 3, 0x1000),
209: 	.bitwidth		= REG_FIELD_ID(0x1000, 0, 1, 3, 0x1000),
210: 
211: 	.rdma_dyncclk		= REG_FIELD_ID(0xC000, 21, 21, 5, 0x1000),
212: 	.rdma_bursten		= REG_FIELD_ID(0xC000, 20, 20, 5, 0x1000),
213: 	.rdma_wpscnt		= REG_FIELD_ID(0xC000, 16, 19, 5, 0x1000),
214: 	.rdma_intf			= REG_FIELD_ID(0xC000, 12, 15, 5, 0x1000),
215: 	.rdma_fifowm		= REG_FIELD_ID(0xC000, 1, 5, 5, 0x1000),
216: 	.rdma_enable		= REG_FIELD_ID(0xC000, 0, 0, 5, 0x1000),
217: 
218: 	.wrdma_dyncclk		= REG_FIELD_ID(0x18000, 22, 22, 4, 0x1000),
219: 	.wrdma_bursten		= REG_FIELD_ID(0x18000, 21, 21, 4, 0x1000),
220: 	.wrdma_wpscnt		= REG_FIELD_ID(0x18000, 17, 20, 4, 0x1000),
221: 	.wrdma_intf		= REG_FIELD_ID(0x18000, 12, 16, 4, 0x1000),
222: 	.wrdma_fifowm		= REG_FIELD_ID(0x18000, 1, 5, 4, 0x1000),
223: 	.wrdma_enable		= REG_FIELD_ID(0x18000, 0, 0, 4, 0x1000),
```

[原始 L283–L300](https://github.com/torvalds/linux/blob/36aad21d50d1af3f6954bb0c988257a908202477/sound/soc/qcom/lpass-sc7180.c#L283-L300)

```text
283: 	.clk_name		= (const char*[]) {
284: 				   "pcnoc-sway-clk",
285: 				   "audio-core",
286: 				   "pcnoc-mport-clk",
287: 				},
288: 	.num_clks		= 3,
289: 	.dai_driver		= sc7180_lpass_cpu_dai_driver,
290: 	.num_dai		= ARRAY_SIZE(sc7180_lpass_cpu_dai_driver),
291: 	.dai_osr_clk_names      = (const char *[]) {
292: 				   "mclk0",
293: 				   "null",
294: 				},
295: 	.dai_bit_clk_names      = (const char *[]) {
296: 				   "mi2s-bit-clk0",
297: 				   "mi2s-bit-clk1",
298: 				},
299: 	.init			= sc7180_lpass_init,
300: 	.exit			= sc7180_lpass_exit,
```

### 7. 寄存器地址 helper 与中断位

[sound/soc/qcom/lpass-lpaif-reg.h](https://github.com/torvalds/linux/blob/36aad21d50d1af3f6954bb0c988257a908202477/sound/soc/qcom/lpass-lpaif-reg.h)  
Blob: `6d9d9d1f6a4d94905639dbea4a02b5cc3273365e`  
原文件许可标识：`/* SPDX-License-Identifier: GPL-2.0-only */`

批注：IRQ 使用全局通道5，寄存器寻址却先减去 wrdma_channel_start=5。只读 STAT，不发明一个 RAW 寄存器来替代 Linux 的语义。

[原始 L82–L86](https://github.com/torvalds/linux/blob/36aad21d50d1af3f6954bb0c988257a908202477/sound/soc/qcom/lpass-lpaif-reg.h#L82-L86)

```text
82: #define LPAIF_RXTX_IRQSTAT_REG(v, port) LPAIF_RXTX_IRQ_REG_ADDR(v, 0x4, port)
83: #define LPAIF_RXTX_IRQCLEAR_REG(v, port) LPAIF_RXTX_IRQ_REG_ADDR(v, 0xC, port)
84: 
85: /* LPAIF VA IRQ */
86: #define LPAIF_VA_IRQ_REG_ADDR(v, addr, port) \
```

[原始 L100–L106](https://github.com/torvalds/linux/blob/36aad21d50d1af3f6954bb0c988257a908202477/sound/soc/qcom/lpass-lpaif-reg.h#L100-L106)

```text
100: #define LPAIF_IRQ_BITSTRIDE		3
101: 
102: #define LPAIF_IRQ_PER(chan)		(1 << (LPAIF_IRQ_BITSTRIDE * (chan)))
103: #define LPAIF_IRQ_XRUN(chan)		(2 << (LPAIF_IRQ_BITSTRIDE * (chan)))
104: #define LPAIF_IRQ_ERR(chan)		(4 << (LPAIF_IRQ_BITSTRIDE * (chan)))
105: 
106: #define LPAIF_IRQ_ALL(chan)		(7 << (LPAIF_IRQ_BITSTRIDE * (chan)))
```

[原始 L137–L148](https://github.com/torvalds/linux/blob/36aad21d50d1af3f6954bb0c988257a908202477/sound/soc/qcom/lpass-lpaif-reg.h#L137-L148)

```text
137: 	(v->wrdma_reg_base + (addr) + \
138: 	 v->wrdma_reg_stride * (chan - v->wrdma_channel_start))
139: 
140: #define LPAIF_WRDMACTL_REG(v, chan)	LPAIF_WRDMA_REG_ADDR(v, 0x00, (chan))
141: #define LPAIF_WRDMABASE_REG(v, chan)	LPAIF_WRDMA_REG_ADDR(v, 0x04, (chan))
142: #define	LPAIF_WRDMABUFF_REG(v, chan)	LPAIF_WRDMA_REG_ADDR(v, 0x08, (chan))
143: #define LPAIF_WRDMACURR_REG(v, chan)	LPAIF_WRDMA_REG_ADDR(v, 0x0C, (chan))
144: #define	LPAIF_WRDMAPER_REG(v, chan)	LPAIF_WRDMA_REG_ADDR(v, 0x10, (chan))
145: #define	LPAIF_WRDMAPERCNT_REG(v, chan)	LPAIF_WRDMA_REG_ADDR(v, 0x14, (chan))
146: 
147: #define LPAIF_INTFDMA_REG(v, chan, reg, dai_id)  \
148: 	((dai_id ==  LPASS_DP_RX) ? \
```

[原始 L267–L269](https://github.com/torvalds/linux/blob/36aad21d50d1af3f6954bb0c988257a908202477/sound/soc/qcom/lpass-lpaif-reg.h#L267-L269)

```text
267: #define LPAIF_DMACTL_WPSCNT_ONE		0
268: #define LPAIF_DMACTL_WPSCNT_TWO		1
269: #define LPAIF_DMACTL_WPSCNT_THREE	2
```

[原始 L284–L288](https://github.com/torvalds/linux/blob/36aad21d50d1af3f6954bb0c988257a908202477/sound/soc/qcom/lpass-lpaif-reg.h#L284-L288)

```text
284: #define LPAIF_DMACTL_FIFOWM_5		4
285: #define LPAIF_DMACTL_FIFOWM_6		5
286: #define LPAIF_DMACTL_FIFOWM_7		6
287: #define LPAIF_DMACTL_FIFOWM_8		7
288: #define LPAIF_DMACTL_FIFOWM_9		8
```

### 8. CPU DAI 的时钟与方向控制

[sound/soc/qcom/lpass-cpu.c](https://github.com/torvalds/linux/blob/36aad21d50d1af3f6954bb0c988257a908202477/sound/soc/qcom/lpass-cpu.c)  
Blob: `ee187eeb739203e3b55113e9401bdc2d9288b1ee`  
原文件许可标识：`// SPDX-License-Identifier: GPL-2.0-only`

批注：数据宽度决定 BCLK；双声道32位就是 rate×32×2。共享 OSR/父时钟不由本驱动关掉；停止只关闭 Primary 的 MICEN/BCLK。

[原始 L83–L103](https://github.com/torvalds/linux/blob/36aad21d50d1af3f6954bb0c988257a908202477/sound/soc/qcom/lpass-cpu.c#L83-L103)

```text
83: static int lpass_cpu_daiops_startup(struct snd_pcm_substream *substream,
84: 		struct snd_soc_dai *dai)
85: {
86: 	struct lpass_data *drvdata = snd_soc_dai_get_drvdata(dai);
87: 	int ret;
88: 
89: 	ret = clk_prepare_enable(drvdata->mi2s_osr_clk[dai->driver->id]);
90: 	if (ret) {
91: 		dev_err(dai->dev, "error in enabling mi2s osr clk: %d\n", ret);
92: 		return ret;
93: 	}
94: 	ret = clk_prepare(drvdata->mi2s_bit_clk[dai->driver->id]);
95: 	if (ret) {
96: 		dev_err(dai->dev, "error in enabling mi2s bit clk: %d\n", ret);
97: 		clk_disable_unprepare(drvdata->mi2s_osr_clk[dai->driver->id]);
98: 		return ret;
99: 	}
100: 	return 0;
101: }
102: 
103: static void lpass_cpu_daiops_shutdown(struct snd_pcm_substream *substream,
```

[原始 L253–L293](https://github.com/torvalds/linux/blob/36aad21d50d1af3f6954bb0c988257a908202477/sound/soc/qcom/lpass-cpu.c#L253-L293)

```text
253: 		ret = regmap_fields_write(i2sctl->spkmode, id,
254: 					 LPAIF_I2SCTL_SPKMODE(mode));
255: 		if (ret) {
256: 			dev_err(dai->dev, "error writing to i2sctl spkr mode: %d\n",
257: 				ret);
258: 			return ret;
259: 		}
260: 		if (channels >= 2)
261: 			ret = regmap_fields_write(i2sctl->spkmono, id,
262: 						 LPAIF_I2SCTL_SPKMONO_STEREO);
263: 		else
264: 			ret = regmap_fields_write(i2sctl->spkmono, id,
265: 						 LPAIF_I2SCTL_SPKMONO_MONO);
266: 	} else {
267: 		ret = regmap_fields_write(i2sctl->micmode, id,
268: 					 LPAIF_I2SCTL_MICMODE(mode));
269: 		if (ret) {
270: 			dev_err(dai->dev, "error writing to i2sctl mic mode: %d\n",
271: 				ret);
272: 			return ret;
273: 		}
274: 		if (channels >= 2)
275: 			ret = regmap_fields_write(i2sctl->micmono, id,
276: 						 LPAIF_I2SCTL_MICMONO_STEREO);
277: 		else
278: 			ret = regmap_fields_write(i2sctl->micmono, id,
279: 						 LPAIF_I2SCTL_MICMONO_MONO);
280: 	}
281: 
282: 	if (ret) {
283: 		dev_err(dai->dev, "error writing to i2sctl channels mode: %d\n",
284: 			ret);
285: 		return ret;
286: 	}
287: 
288: 	ret = clk_set_rate(drvdata->mi2s_bit_clk[id],
289: 			   rate * bitwidth * 2);
290: 	if (ret) {
291: 		dev_err(dai->dev, "error setting mi2s bitclk to %u: %d\n",
292: 			rate * bitwidth * 2, ret);
293: 		return ret;
```

[原始 L320–L338](https://github.com/torvalds/linux/blob/36aad21d50d1af3f6954bb0c988257a908202477/sound/soc/qcom/lpass-cpu.c#L320-L338)

```text
320: 		 *     turn off the shared BCLK while other devices are using
321: 		 *     it.
322: 		 */
323: 		if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
324: 			ret = regmap_fields_write(i2sctl->spken, id,
325: 						 LPAIF_I2SCTL_SPKEN_ENABLE);
326: 		} else  {
327: 			ret = regmap_fields_write(i2sctl->micen, id,
328: 						 LPAIF_I2SCTL_MICEN_ENABLE);
329: 		}
330: 		if (ret)
331: 			dev_err(dai->dev, "error writing to i2sctl reg: %d\n",
332: 				ret);
333: 
334: 		ret = clk_enable(drvdata->mi2s_bit_clk[id]);
335: 		if (ret) {
336: 			dev_err(dai->dev, "error in enabling mi2s bit clk: %d\n", ret);
337: 			clk_disable(drvdata->mi2s_osr_clk[id]);
338: 			return ret;
```

### 9. DMA 大小、WPSCNT、长度和 IRQ 行为

[sound/soc/qcom/lpass-platform.c](https://github.com/torvalds/linux/blob/36aad21d50d1af3f6954bb0c988257a908202477/sound/soc/qcom/lpass-platform.c)  
Blob: `e162627d6f86e5448959e9fb146c5b65327751fd`  
原文件许可标识：`// SPDX-License-Identifier: GPL-2.0-only`

批注：长度寄存器单位是32位字减1，非 frames。48 KiB 是6144个 stereo-S32 frames，不是8192或65536。Linux 的 XRUN/ERR 停机语义在这里以错误状态与显式重启表达。

[原始 L22–L24](https://github.com/torvalds/linux/blob/36aad21d50d1af3f6954bb0c988257a908202477/sound/soc/qcom/lpass-platform.c#L22-L24)

```text
22: #define LPASS_PLATFORM_BUFFER_SIZE	(24 *  2 * 1024)
23: #define LPASS_PLATFORM_PERIODS		2
24: #define LPASS_RXTX_CDC_DMA_LPM_BUFF_SIZE (8 * 1024)
```

[原始 L444–L454](https://github.com/torvalds/linux/blob/36aad21d50d1af3f6954bb0c988257a908202477/sound/soc/qcom/lpass-platform.c#L444-L454)

```text
444: 	ret = regmap_fields_write(dmactl->bursten, id, LPAIF_DMACTL_BURSTEN_INCR4);
445: 	if (ret) {
446: 		dev_err(soc_runtime->dev, "error updating bursten field: %d\n", ret);
447: 		return ret;
448: 	}
449: 
450: 	ret = regmap_fields_write(dmactl->fifowm, id, LPAIF_DMACTL_FIFOWM_8);
451: 	if (ret) {
452: 		dev_err(soc_runtime->dev, "error updating fifowm field: %d\n", ret);
453: 		return ret;
454: 	}
```

[原始 L520–L541](https://github.com/torvalds/linux/blob/36aad21d50d1af3f6954bb0c988257a908202477/sound/soc/qcom/lpass-platform.c#L520-L541)

```text
520: 		break;
521: 	case 24:
522: 	case 32:
523: 		switch (channels) {
524: 		case 1:
525: 			regval = LPAIF_DMACTL_WPSCNT_ONE;
526: 			break;
527: 		case 2:
528: 			regval = (dai_id == LPASS_DP_RX ?
529: 			LPAIF_DMACTL_WPSCNT_ONE :
530: 			LPAIF_DMACTL_WPSCNT_TWO);
531: 			break;
532: 		case 4:
533: 			regval = (dai_id == LPASS_DP_RX ?
534: 			LPAIF_DMACTL_WPSCNT_TWO :
535: 			LPAIF_DMACTL_WPSCNT_FOUR);
536: 			break;
537: 		case 6:
538: 			regval = (dai_id == LPASS_DP_RX ?
539: 			LPAIF_DMACTL_WPSCNT_THREE :
540: 			LPAIF_DMACTL_WPSCNT_SIX);
541: 			break;
```

[原始 L618–L636](https://github.com/torvalds/linux/blob/36aad21d50d1af3f6954bb0c988257a908202477/sound/soc/qcom/lpass-platform.c#L618-L636)

```text
618: 				runtime->dma_addr);
619: 	if (ret) {
620: 		dev_err(soc_runtime->dev, "error writing to rdmabase reg: %d\n",
621: 			ret);
622: 		return ret;
623: 	}
624: 
625: 	ret = regmap_write(map, LPAIF_DMABUFF_REG(v, ch, dir, dai_id),
626: 			(snd_pcm_lib_buffer_bytes(substream) >> 2) - 1);
627: 	if (ret) {
628: 		dev_err(soc_runtime->dev, "error writing to rdmabuff reg: %d\n",
629: 			ret);
630: 		return ret;
631: 	}
632: 
633: 	ret = regmap_write(map, LPAIF_DMAPER_REG(v, ch, dir, dai_id),
634: 			(snd_pcm_lib_period_bytes(substream) >> 2) - 1);
635: 	if (ret) {
636: 		dev_err(soc_runtime->dev, "error writing to rdmaper reg: %d\n",
```

[原始 L954–L995](https://github.com/torvalds/linux/blob/36aad21d50d1af3f6954bb0c988257a908202477/sound/soc/qcom/lpass-platform.c#L954-L995)

```text
954: 		val = 0;
955: 	break;
956: 	case LPASS_CDC_DMA_RX0 ... LPASS_CDC_DMA_RX9:
957: 	case LPASS_CDC_DMA_TX0 ... LPASS_CDC_DMA_TX8:
958: 		map = drvdata->rxtx_lpaif_map;
959: 		reg = LPAIF_RXTX_IRQCLEAR_REG(v, LPAIF_IRQ_PORT_HOST);
960: 		val = 0;
961: 	break;
962: 	case LPASS_CDC_DMA_VA_TX0 ... LPASS_CDC_DMA_VA_TX8:
963: 		map = drvdata->va_lpaif_map;
964: 		reg = LPAIF_VA_IRQCLEAR_REG(v, LPAIF_IRQ_PORT_HOST);
965: 		val = 0;
966: 	break;
967: 	default:
968: 	dev_err(soc_runtime->dev, "%s: invalid  %d interface\n", __func__, dai_id);
969: 	return -EINVAL;
970: 	}
971: 	if (interrupts & LPAIF_IRQ_PER(chan)) {
972: 		rv = regmap_write_bits(map, reg, mask, (LPAIF_IRQ_PER(chan) | val));
973: 		if (rv) {
974: 			dev_err(soc_runtime->dev,
975: 				"error writing to irqclear reg: %d\n", rv);
976: 			return IRQ_NONE;
977: 		}
978: 		snd_pcm_period_elapsed(substream);
979: 		ret = IRQ_HANDLED;
980: 	}
981: 
982: 	if (interrupts & LPAIF_IRQ_XRUN(chan)) {
983: 		rv = regmap_write_bits(map, reg, mask, (LPAIF_IRQ_XRUN(chan) | val));
 		if (rv) {
985: 			dev_err(soc_runtime->dev,
986: 				"error writing to irqclear reg: %d\n", rv);
987: 			return IRQ_NONE;
988: 		}
989: 		dev_warn_ratelimited(soc_runtime->dev, "xrun warning\n");
990: 
991: 		snd_pcm_stop_xrun(substream);
992: 		ret = IRQ_HANDLED;
993: 	}
994: 
995: 	if (interrupts & LPAIF_IRQ_ERR(chan)) {
```

### 10. Primary RCG 频表与 branch

[drivers/clk/qcom/lpasscorecc-sc7180.c](https://github.com/torvalds/linux/blob/36aad21d50d1af3f6954bb0c988257a908202477/drivers/clk/qcom/lpasscorecc-sc7180.c)  
Blob: `af28da5f04b37e649bf02c4da9f983784e6d9f43`  
原文件许可标识：`// SPDX-License-Identifier: GPL-2.0-only`

批注：共享 PLL L=0x20、odd postdivider=5，19.2 MHz×32/5=122.88 MHz。3.072 MHz 行使用 prediv10、M=1/N=4。Primary RCG 在0x10000，Secondary 在0x11000，禁止交叉写。

[原始 L34–L44](https://github.com/torvalds/linux/blob/36aad21d50d1af3f6954bb0c988257a908202477/drivers/clk/qcom/lpasscorecc-sc7180.c#L34-L44)

```text
34: 	.l = 0x20,
35: 	.alpha = 0x0,
36: 	.config_ctl_val = 0x20485699,
37: 	.config_ctl_hi_val = 0x00002067,
38: 	.test_ctl_val = 0x40000000,
39: 	.test_ctl_hi_val = 0x00000000,
40: 	.user_ctl_val = 0x00005105,
41: 	.user_ctl_hi_val = 0x00004805,
42: };
43: 
44: static const u8 clk_alpha_pll_regs_offset[][PLL_OFF_MAX_REGS] = {
```

[原始 L79–L95](https://github.com/torvalds/linux/blob/36aad21d50d1af3f6954bb0c988257a908202477/drivers/clk/qcom/lpasscorecc-sc7180.c#L79-L95)

```text
79: static const struct clk_div_table
80: 			post_div_table_lpass_lpaaudio_dig_pll_out_odd[] = {
81: 	{ 0x5, 5 },
82: 	{ }
83: };
84: 
85: static struct clk_alpha_pll_postdiv lpass_lpaaudio_dig_pll_out_odd = {
86: 	.offset = 0x1000,
87: 	.post_div_shift = 12,
88: 	.post_div_table = post_div_table_lpass_lpaaudio_dig_pll_out_odd,
89: 	.num_post_div =
90: 		ARRAY_SIZE(post_div_table_lpass_lpaaudio_dig_pll_out_odd),
91: 	.width = 4,
92: 	.regs = clk_alpha_pll_regs[CLK_ALPHA_PLL_TYPE_FABIA],
93: 	.clkr.hw.init = &(struct clk_init_data){
94: 		.name = "lpass_lpaaudio_dig_pll_out_odd",
95: 		.parent_hws = (const struct clk_hw*[]) {
```

[原始 L104–L107](https://github.com/torvalds/linux/blob/36aad21d50d1af3f6954bb0c988257a908202477/drivers/clk/qcom/lpasscorecc-sc7180.c#L104-L107)

```text
104: static const struct parent_map lpass_core_cc_parent_map_0[] = {
105: 	{ P_BI_TCXO, 0 },
106: 	{ P_LPASS_LPAAUDIO_DIG_PLL_OUT_ODD, 5 },
107: };
```

[原始 L138–L150](https://github.com/torvalds/linux/blob/36aad21d50d1af3f6954bb0c988257a908202477/drivers/clk/qcom/lpasscorecc-sc7180.c#L138-L150)

```text
138: 
139: static const struct freq_tbl ftbl_ext_lpaif_clk_src[] = {
140: 	F(256000, P_LPASS_LPAAUDIO_DIG_PLL_OUT_ODD, 15, 1, 32),
141: 	F(512000, P_LPASS_LPAAUDIO_DIG_PLL_OUT_ODD, 15, 1, 16),
142: 	F(768000, P_LPASS_LPAAUDIO_DIG_PLL_OUT_ODD, 10, 1, 16),
143: 	F(1024000, P_LPASS_LPAAUDIO_DIG_PLL_OUT_ODD, 15, 1, 8),
144: 	F(1536000, P_LPASS_LPAAUDIO_DIG_PLL_OUT_ODD, 10, 1, 8),
145: 	F(2048000, P_LPASS_LPAAUDIO_DIG_PLL_OUT_ODD, 15, 1, 4),
146: 	F(3072000, P_LPASS_LPAAUDIO_DIG_PLL_OUT_ODD, 10, 1, 4),
147: 	F(4096000, P_LPASS_LPAAUDIO_DIG_PLL_OUT_ODD, 15, 1, 2),
148: 	F(6144000, P_LPASS_LPAAUDIO_DIG_PLL_OUT_ODD, 10, 1, 2),
149: 	F(8192000, P_LPASS_LPAAUDIO_DIG_PLL_OUT_ODD, 15, 0, 0),
150: 	F(9600000, P_BI_TCXO, 2, 0, 0),
```

[原始 L172–L188](https://github.com/torvalds/linux/blob/36aad21d50d1af3f6954bb0c988257a908202477/drivers/clk/qcom/lpasscorecc-sc7180.c#L172-L188)

```text
172: static struct clk_rcg2 lpaif_pri_clk_src = {
173: 	.cmd_rcgr = 0x10000,
174: 	.mnd_width = 16,
175: 	.hid_width = 5,
176: 	.parent_map = lpass_core_cc_parent_map_0,
177: 	.freq_tbl = ftbl_ext_lpaif_clk_src,
178: 	.clkr.hw.init = &(struct clk_init_data){
179: 		.name = "lpaif_pri_clk_src",
180: 		.parent_data = lpass_core_cc_parent_data_0,
181: 		.num_parents = 2,
182: 		.flags = CLK_SET_RATE_PARENT,
183: 		.ops = &clk_rcg2_ops,
184: 	},
185: };
186: 
187: static struct clk_rcg2 lpaif_sec_clk_src = {
188: 	.cmd_rcgr = 0x11000,
```

[原始 L222–L238](https://github.com/torvalds/linux/blob/36aad21d50d1af3f6954bb0c988257a908202477/drivers/clk/qcom/lpasscorecc-sc7180.c#L222-L238)

```text
222: static struct clk_branch lpass_audio_core_lpaif_pri_ibit_clk = {
223: 	.halt_reg = 0x10018,
224: 	.halt_check = BRANCH_HALT,
225: 	.hwcg_reg = 0x10018,
226: 	.hwcg_bit = 1,
227: 	.clkr = {
228: 		.enable_reg = 0x10018,
229: 		.enable_mask = BIT(0),
230: 		.hw.init = &(struct clk_init_data){
231: 			.name = "lpass_audio_core_lpaif_pri_ibit_clk",
232: 			.parent_hws = (const struct clk_hw*[]) {
233: 				&lpaif_pri_clk_src.clkr.hw,
234: 			},
235: 			.num_parents = 1,
236: 			.flags = CLK_SET_RATE_PARENT,
237: 			.ops = &clk_branch2_ops,
238: 		},
```

### 11. 必须读到 M/N/D helper 实现

[drivers/clk/qcom/clk-rcg2.c](https://github.com/torvalds/linux/blob/36aad21d50d1af3f6954bb0c988257a908202477/drivers/clk/qcom/clk-rcg2.c)  
Blob: `c44c51d860e1fc464ae54755d0f43c8cae1ce236`  
原文件许可标识：`// SPDX-License-Identifier: GPL-2.0`

批注：N 不是直接写4，而是 ~(N-M)；D 使用 helper 中的 clamp 后取反。故本路径 M=1、Nreg=0xfffc、Dreg=0xfffb；CFG 的 predivider 是 HID=19。更新后等待 CMD_UPDATE 清零。

[原始 L114–L139](https://github.com/torvalds/linux/blob/36aad21d50d1af3f6954bb0c988257a908202477/drivers/clk/qcom/clk-rcg2.c#L114-L139)

```text
114: static int update_config(struct clk_rcg2 *rcg)
115: {
116: 	int count, ret;
117: 	u32 cmd;
118: 	struct clk_hw *hw = &rcg->clkr.hw;
119: 	const char *name = clk_hw_get_name(hw);
120: 
121: 	ret = regmap_update_bits(rcg->clkr.regmap, rcg->cmd_rcgr + CMD_REG,
122: 				 CMD_UPDATE, CMD_UPDATE);
123: 	if (ret)
124: 		return ret;
125: 
126: 	/* Wait for update to take effect */
127: 	for (count = 500; count > 0; count--) {
128: 		ret = regmap_read(rcg->clkr.regmap, rcg->cmd_rcgr + CMD_REG, &cmd);
129: 		if (ret)
130: 			return ret;
131: 		if (!(cmd & CMD_UPDATE))
132: 			return 0;
133: 		udelay(1);
134: 	}
135: 
136: 	WARN(1, "%s: rcg didn't update its configuration.", name);
137: 	return -EBUSY;
138: }
139: 
```

[原始 L517–L562](https://github.com/torvalds/linux/blob/36aad21d50d1af3f6954bb0c988257a908202477/drivers/clk/qcom/clk-rcg2.c#L517-L562)

```text
517: static int __clk_rcg2_configure_mnd(struct clk_rcg2 *rcg, const struct freq_tbl *f,
518: 				u32 *_cfg)
519: {
520: 	u32 cfg, mask, d_val, not2d_val, n_minus_m;
521: 	int ret;
522: 
523: 	if (rcg->mnd_width && f->n) {
524: 		mask = BIT(rcg->mnd_width) - 1;
525: 		ret = regmap_update_bits(rcg->clkr.regmap,
526: 				RCG_M_OFFSET(rcg), mask, f->m);
527: 		if (ret)
528: 			return ret;
529: 
530: 		ret = regmap_update_bits(rcg->clkr.regmap,
531: 				RCG_N_OFFSET(rcg), mask, ~(f->n - f->m));
532: 		if (ret)
533: 			return ret;
534: 
535: 		/* Calculate 2d value */
536: 		d_val = f->n;
537: 
538: 		n_minus_m = f->n - f->m;
539: 		n_minus_m *= 2;
540: 
541: 		d_val = clamp_t(u32, d_val, f->m, n_minus_m);
542: 		not2d_val = ~d_val & mask;
543: 
544: 		ret = regmap_update_bits(rcg->clkr.regmap,
545: 				RCG_D_OFFSET(rcg), mask, not2d_val);
546: 		if (ret)
547: 			return ret;
548: 	}
549: 
550: 	mask = BIT(rcg->hid_width) - 1;
551: 	mask |= CFG_MODE_MASK | CFG_HW_CLK_CTRL_MASK;
552: 	cfg = f->pre_div << CFG_SRC_DIV_SHIFT;
553: 	if (rcg->mnd_width && f->n && (f->m != f->n))
554: 		cfg |= CFG_MODE_DUAL_EDGE;
555: 	if (rcg->hw_clk_ctrl)
556: 		cfg |= CFG_HW_CLK_CTRL_MASK;
557: 
558: 	*_cfg &= ~mask;
559: 	*_cfg |= cfg;
560: 
561: 	return 0;
562: }
```

### 12. TLMM 字段、tile 与 mux 编号

[drivers/pinctrl/qcom/pinctrl-sc7180.c](https://github.com/torvalds/linux/blob/36aad21d50d1af3f6954bb0c988257a908202477/drivers/pinctrl/qcom/pinctrl-sc7180.c)  
Blob: `a02a9a78a557f02272689f25618aa7141a273338`  
原文件许可标识：`// SPDX-License-Identifier: GPL-2.0`

批注：引脚位于 WEST；函数数组中的 gpio 占 mux0，因此 mi2s_0/lpass_ext 都是 mux1。OE 是 bit9，不在本实现的0x1ff更新掩码中。

[原始 L27–L51](https://github.com/torvalds/linux/blob/36aad21d50d1af3f6954bb0c988257a908202477/drivers/pinctrl/qcom/pinctrl-sc7180.c#L27-L51)

```text
27: 		.funcs = (int[]){			\
28: 			msm_mux_gpio, /* gpio mode */	\
29: 			msm_mux_##f1,			\
30: 			msm_mux_##f2,			\
31: 			msm_mux_##f3,			\
32: 			msm_mux_##f4,			\
33: 			msm_mux_##f5,			\
34: 			msm_mux_##f6,			\
35: 			msm_mux_##f7,			\
36: 			msm_mux_##f8,			\
37: 			msm_mux_##f9			\
38: 		},					\
39: 		.nfuncs = 10,				\
40: 		.ctl_reg = 0x1000 * id,		\
41: 		.io_reg = 0x1000 * id + 0x4,		\
42: 		.intr_cfg_reg = 0x1000 * id + 0x8,	\
43: 		.intr_status_reg = 0x1000 * id + 0xc,	\
44: 		.tile = _tile,			\
45: 		.mux_bit = 2,			\
46: 		.pull_bit = 0,			\
47: 		.drv_bit = 6,			\
48: 		.oe_bit = 9,			\
49: 		.in_bit = 0,			\
50: 		.out_bit = 1,			\
51: 		.intr_enable_bit = 0,		\
```

[原始 L1035–L1039](https://github.com/torvalds/linux/blob/36aad21d50d1af3f6954bb0c988257a908202477/drivers/pinctrl/qcom/pinctrl-sc7180.c#L1035-L1039)

```text
1035: 	[53] = PINGROUP(53, WEST, mi2s_0, qup15, qdss, atest_usb22, _, _, _, _, _),
1036: 	[54] = PINGROUP(54, WEST, mi2s_0, qup15, qdss, atest_usb21, _, _, _, _, _),
1037: 	[55] = PINGROUP(55, WEST, mi2s_0, qup15, qdss, atest_usb20, _, _, _, _, _),
1038: 	[56] = PINGROUP(56, WEST, mi2s_0, qup15, gcc_gp1, _, phase_flag, qdss, _, _, _),
1039: 	[57] = PINGROUP(57, WEST, lpass_ext, audio_ref, jitter_bist, gp_pdm2, _, phase_flag, qdss, _, _),
```

[原始 L1065–L1065](https://github.com/torvalds/linux/blob/36aad21d50d1af3f6954bb0c988257a908202477/drivers/pinctrl/qcom/pinctrl-sc7180.c#L1065-L1065)

```text
1065: 	[83] = PINGROUP(83, WEST, _, _V_GPIO, _V_PPS_IN, _V_PPS_OUT, gps_tx, _, _, _, _),
```

### 13. pinctrl 参数也要读 helper

[drivers/pinctrl/qcom/pinctrl-msm.c](https://github.com/torvalds/linux/blob/36aad21d50d1af3f6954bb0c988257a908202477/drivers/pinctrl/qcom/pinctrl-msm.c)  
Blob: `27f06bbc113c96e0b39fe5d464d6fae3c125c025`  
原文件许可标识：`// SPDX-License-Identifier: GPL-2.0-only`

批注：pull-down=1，pull-up=3；drive 编码是(mA/2)-1。因此2mA=0，8mA=3，不是把 mA 数字直接写进位域。

[原始 L328–L336](https://github.com/torvalds/linux/blob/36aad21d50d1af3f6954bb0c988257a908202477/drivers/pinctrl/qcom/pinctrl-msm.c#L328-L336)

```text
328: #define MSM_KEEPER		2
329: #define MSM_PULL_UP_NO_KEEPER	2
330: #define MSM_PULL_UP		3
331: #define MSM_I2C_STRONG_PULL_UP	2200
332: 
333: static unsigned msm_regval_to_drive(u32 val)
334: {
335: 	return (val + 1) * 2;
336: }
```

[原始 L458–L481](https://github.com/torvalds/linux/blob/36aad21d50d1af3f6954bb0c988257a908202477/drivers/pinctrl/qcom/pinctrl-msm.c#L458-L481)

```text
458: 		case PIN_CONFIG_BIAS_BUS_HOLD:
459: 			if (pctrl->soc->pull_no_keeper)
460: 				return -ENOTSUPP;
461: 
462: 			arg = MSM_KEEPER;
463: 			break;
464: 		case PIN_CONFIG_BIAS_PULL_UP:
465: 			if (pctrl->soc->pull_no_keeper)
466: 				arg = MSM_PULL_UP_NO_KEEPER;
467: 			else if (g->i2c_pull_bit && arg == MSM_I2C_STRONG_PULL_UP)
468: 				arg = BIT(g->i2c_pull_bit) | MSM_PULL_UP;
469: 			else
470: 				arg = MSM_PULL_UP;
471: 			break;
472: 		case PIN_CONFIG_DRIVE_OPEN_DRAIN:
473: 			arg = 1;
474: 			break;
475: 		case PIN_CONFIG_DRIVE_STRENGTH:
476: 			/* Check for invalid values */
477: 			if (arg > 16 || arg < 2 || (arg % 2) != 0)
478: 				arg = -1;
479: 			else
480: 				arg = (arg / 2) - 1;
481: 			break;
```

### 14. regulator 模式编码

[drivers/regulator/qcom-rpmh-regulator.c](https://github.com/torvalds/linux/blob/36aad21d50d1af3f6954bb0c988257a908202477/drivers/regulator/qcom-rpmh-regulator.c)  
Blob: `4c90d206e63bc1651f74569da1c0234497206264`  
原文件许可标识：`// SPDX-License-Identifier: GPL-2.0`

批注：PMIC5 LDO HPM 是7。驱动通过共享 provider 请求 ldoa15/1800mV/HPM，不使用裸 PMIC 写入或复制另一颗芯片的上电序列。

[原始 L85–L91](https://github.com/torvalds/linux/blob/36aad21d50d1af3f6954bb0c988257a908202477/drivers/regulator/qcom-rpmh-regulator.c#L85-L91)

```text
85: #define PMIC4_BOB_MODE_PWM			3
86: 
87: #define PMIC5_LDO_MODE_RETENTION		3
88: #define PMIC5_LDO_MODE_LPM			4
89: #define PMIC5_LDO_MODE_HPM			7
90: 
91: #define PMIC5_SMPS_MODE_RETENTION		3
```

[原始 L665–L672](https://github.com/torvalds/linux/blob/36aad21d50d1af3f6954bb0c988257a908202477/drivers/regulator/qcom-rpmh-regulator.c#L665-L672)

```text
665: static const int pmic_mode_map_pmic5_ldo[REGULATOR_MODE_STANDBY + 1] = {
666: 	[REGULATOR_MODE_INVALID] = -EINVAL,
667: 	[REGULATOR_MODE_STANDBY] = PMIC5_LDO_MODE_RETENTION,
668: 	[REGULATOR_MODE_IDLE]    = PMIC5_LDO_MODE_LPM,
669: 	[REGULATOR_MODE_NORMAL]  = PMIC5_LDO_MODE_HPM,
670: 	[REGULATOR_MODE_FAST]    = -EINVAL,
671: };
672: 
```

## XNU 系统音频参考

- [UniXNU DSP、音频与电源 wiki](https://unixnu.com/zh/porting/power)：本次读取其音频传输、缓冲、系统服务与验收章节。用于 OS 接口和生命周期；其中 SM8550 AudioReach/SoundWire 硬件序列不用于 Coachz。
- [ShengAudioService.cpp，552b67499135bae191f415f444d782a6942c252e](https://github.com/UniXNU/UniXNU-sheng/blob/552b67499135bae191f415f444d782a6942c252e/package/ShengAudioService.cpp)：核对三块共享内存、请求转发和 clientClose 同时停止输入/输出的接口。它调用 sheng_audio_request，硬件实现另属 AudioProbe；本驱动不复制其 ADSP 端点。
- [ShengAudioPlugIn.c，同一固定版本](https://github.com/UniXNU/UniXNU-sheng/blob/552b67499135bae191f415f444d782a6942c252e/hal/ShengAudioPlugIn.c)：核对 HAL 输入方向和 ring 接口。当前项目的 Coachz HAL 已有可复用输入 stream，本次不替换音频栈。
- [HakiOS drivers wiki](https://hackios-web.vercel.app/en/docs/reference/drivers)：本次查看其 microphone、audio 和平台依赖分层。其麦克风依赖 ADSP，与这台板的直接 LPASS/ADAU7002 路径不同；未从该页面借用硬件配置，也没有把 HakiOS 的编译开关引入本项目。

以上 wiki 为本次访问的网页；固定版本只适用于明确标注 SHA 的源码链接，不声称 wiki 永久不变。

## 当前项目接口核对（仅阅读）

- `platform/coachz-prelink/package/CoachzAudio.cpp`：现有输入 IOBuffer、StartInput/StopInput selector、10 ms worker 和占位输入进度。当前占位输入仍在活动代码中，本次没有改成真实采集。
- `platform/coachz-prelink/package/CoachzAudioAbi.h`：现有 control page、两个 sample rings，诊断位于2048。pending 扩展放1024，不移动既有字段。
- `platform/coachz-prelink/package/CoachzAudioPlugIn.c`：输入已有 mono Float32 格式，但 StartIO 和 IsActive 仍需接入方向仲裁；详见 INTEGRATION.md。
- `platform/coachz-prelink/package/CoachzGpu.cpp`、`drivers/RamBlock/mmc/coachz_sdcc.c`：核对当前 ArmCpu 的 `armcpu_spi_attach` 导出调用约定。没有根据别的内核版本猜测中断注册 API。
- `platform/coachz-prelink/patches/xnu/coachz-prelink-wifi.patch`：核对共享 regulator API 和3个 consumer 的现状；未编辑该文件。

## 尚无运行证据

这些是源码依据和未接入的实现，不是编译通过、有效电源投票、成功的 WRDMA/SMMU 访问或录音实机验收。模型测试也仅写入，未执行。

补充核对了 [Apple XNU IOLocks.h](https://github.com/apple-oss-distributions/xnu/blob/main/iokit/IOKit/IOLocks.h)，本次返回的 blob 为 `79531f24370a2656b83c127f430af7e9737f636a`：`IOSimpleLockAlloc`、`IOSimpleLockLockDisableInterrupt`、`IOSimpleLockUnlockEnableInterrupt` 和 `IOInterruptState`。绑定保留该类型的完整 interrupt state。这个链接是访问时的 main，不能替代以后对项目实际选定 XNU 头文件的编译验证。

## RCG 编码与 branch helper 补充

批注：HID=19 的依据是 F 宏中的 `2*h-1`。branch 不能只把“HALT 位清零”等价为全部成功条件：主线还处理 HWCG 与 NOC FSM_STATUS_ON。pending 实现对照以下 helper，使用200次 branch 轮询和500次 RCG UPDATE 轮询。

[drivers/clk/qcom/clk-rcg.h](https://github.com/torvalds/linux/blob/36aad21d50d1af3f6954bb0c988257a908202477/drivers/clk/qcom/clk-rcg.h)，blob `4fbdf4880d0308714a2d1dc8c510ec3819206b9e`；原许可证：/* SPDX-License-Identifier: GPL-2.0 */。

```text
8: #include "clk-regmap.h"
9: 
10: #define F(f, s, h, m, n) { (f), (s), (2 * (h) - 1), (m), (n) }
11: 
```

[drivers/clk/qcom/clk-branch.h](https://github.com/torvalds/linux/blob/36aad21d50d1af3f6954bb0c988257a908202477/drivers/clk/qcom/clk-branch.h)，blob `6bc2ba2b5350554005b7f0c84f933580b7582fc7`；原许可证：/* SPDX-License-Identifier: GPL-2.0 */。

```text
30: #define BRANCH_VOTED			BIT(7) /* Delay on disable */
31: #define BRANCH_HALT			0 /* pol: 1 = halt */
32: #define BRANCH_HALT_VOTED		(BRANCH_HALT | BRANCH_VOTED)
33: #define BRANCH_HALT_ENABLE		1 /* pol: 0 = halt */
34: #define BRANCH_HALT_ENABLE_VOTED	(BRANCH_HALT_ENABLE | BRANCH_VOTED)
35: #define BRANCH_HALT_DELAY		2 /* No bit to check; just delay */
36: #define BRANCH_HALT_SKIP		3 /* Don't check halt bit */
```

[drivers/clk/qcom/clk-branch.c](https://github.com/torvalds/linux/blob/36aad21d50d1af3f6954bb0c988257a908202477/drivers/clk/qcom/clk-branch.c)，blob `444e7d8648d42ca9acc76bf3b054e3e2a7507aa1`；原许可证：// SPDX-License-Identifier: GPL-2.0。

```text
17: static bool clk_branch_in_hwcg_mode(const struct clk_branch *br)
18: {
19: 	u32 val;
20: 
21: 	if (!br->hwcg_reg)
22: 		return false;
23: 
24: 	regmap_read(br->clkr.regmap, br->hwcg_reg, &val);
25: 
26: 	return !!(val & BIT(br->hwcg_bit));
27: }
```

```text
43: static bool clk_branch2_check_halt(const struct clk_branch *br, bool enabling)
44: {
45: 	u32 val;
46: 	u32 mask;
47: 	bool invert = (br->halt_check & BRANCH_HALT_ENABLE);
48: 
49: 	mask = CBCR_NOC_FSM_STATUS;
50: 	mask |= CBCR_CLK_OFF;
51: 
52: 	regmap_read(br->clkr.regmap, br->halt_reg, &val);
53: 
54: 	if (enabling) {
55: 		val &= mask;
56: 		return (val & CBCR_CLK_OFF) == (invert ? CBCR_CLK_OFF : 0) ||
57: 			FIELD_GET(CBCR_NOC_FSM_STATUS, val) == FSM_STATUS_ON;
58: 	}
59: 	return (val & CBCR_CLK_OFF) == (invert ? 0 : CBCR_CLK_OFF);
60: }
61: 
62: static int clk_branch_wait(const struct clk_branch *br, bool enabling,
63: 		bool (check_halt)(const struct clk_branch *, bool))
64: {
65: 	bool voted = br->halt_check & BRANCH_VOTED;
66: 	const char *name = clk_hw_get_name(&br->clkr.hw);
67: 
68: 	/*
69: 	 * Skip checking halt bit if we're explicitly ignoring the bit or the
70: 	 * clock is in hardware gated mode
71: 	 */
72: 	if (br->halt_check == BRANCH_HALT_SKIP || clk_branch_in_hwcg_mode(br))
73: 		return 0;
74: 
75: 	if (br->halt_check == BRANCH_HALT_DELAY || (!enabling && voted)) {
76: 		udelay(10);
77: 	} else if (br->halt_check == BRANCH_HALT_ENABLE ||
78: 		   br->halt_check == BRANCH_HALT ||
79: 		   (enabling && voted)) {
80: 		int count = 200;
81: 
82: 		while (count-- > 0) {
83: 			if (check_halt(br, enabling))
84: 				return 0;
85: 			udelay(1);
86: 		}
87: 		WARN(1, "%s status stuck at 'o%s'", name,
88: 				enabling ? "ff" : "n");
89: 		return -EBUSY;
90: 	}
91: 	return 0;
```

上面 branch helper 使用的字段（同一 clk-branch.h）：

```text
63: #define CBCR_CLK_OFF			BIT(31)
64: #define CBCR_NOC_FSM_STATUS		GENMASK(30, 28)
65:  #define FSM_STATUS_ON			BIT(1)
```

