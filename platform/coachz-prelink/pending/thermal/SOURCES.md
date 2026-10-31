# TSENS 移植证据与批注

调查／实现日期：2026-11-01。以下是固定版本的源码证据，不是实机结论。现有 UniXNU 实现作为 BSD 移植起点；Linux 只用于核对寄存器事实与 SC7180 布局，没有把 GPL 实现搬进 BSD 文件。

## 1. SC7180 布局

[Linux sc7180.dtsi](https://github.com/torvalds/linux/blob/36aad21d50d1af3f6954bb0c988257a908202477/arch/arm64/boot/dts/qcom/sc7180.dtsi#L3532-L3553)，blob `70fbfb31d4a4dd0d0dd75feb37f686933ff38a14`。

```text
                 TM          SROT       sensors
tsens0       0x0c263000   0x0c222000        15
tsens1       0x0c265000   0x0c223000        10
compatible: qcom,sc7180-tsens / qcom,tsens-v2
```

批注：这是两组独立 bank，不是可互换的两个地址。代码只请求各自存在的通道；没有把其他平台的 16 路默认全部套上，也没有为 sensor ID 擅自命名 CPU／GPU／机壳。

## 2. 版本、使能、就绪与有效位

[Linux tsens-v2.c](https://github.com/torvalds/linux/blob/36aad21d50d1af3f6954bb0c988257a908202477/drivers/thermal/qcom/tsens-v2.c#L83-L138)，blob `843dd29fad2e38e46eabdad9cd7dbbdca0181812`。对应的字段事实摘录：

```text
SROT + 0x00: VERSION major[31:28], minor[27:16], step[15:0]
SROT + 0x04: TSENS_EN[0], SW_RST[1], SENSOR_EN[18:3]
TM   + 0xe4: TRDY[0]
TM   + 0xa0 + sensor*4: LAST_TEMP[11:0], VALID[21]
generic v2: adc = 0
```

批注：主号不是 2 时，当前代码不按 v2 格式解读后续寄存器。与 Linux `get_temp_tsens_valid()` 主要依靠单路有效位不同，这个诊断采样还保守要求 TRDY，保留原始 ready 值供实机确认；因此可能拒绝输出温度，而不是证明固件必定能满足所有条件。没有把 IPQ 无 RPM 路径的 `CODE_OR_TEMP` 初始化额外移植到 SC7180。

## 3. 符号和单位

[Linux tsens.c](https://github.com/torvalds/linux/blob/36aad21d50d1af3f6954bb0c988257a908202477/drivers/thermal/qcom/tsens.c#L386-L402)，blob `4d7013fc79d63f9182f78a82f4a07e0969c1a2a4`：非 ADC 路径对温度字段符号扩展，再乘 100。

```c
/* This implementation's explicit equivalent for a 12-bit result: */
int32_t code = (int32_t)(raw & 0xfff);
if (code & 0x800) code -= 0x1000;
return code * 100;
```

批注：`0xfff` 是 -0.1°C，不是 409.5°C。数值存储为零时仍必须检查状态；只有 `CT_OK` 才能消费，峰值也只取本轮有效通道。

## 4. UniXNU 起点及明确修订

[NativeQcomTsens.cpp](https://github.com/RigoLigoRLC/unixnu-nativeqcomtsens/blob/8b2fb5e610eaed8fb7c0dc368da9e379adc0e12f/NativeQcomTsens.cpp)，blob `ce3b97571aa490df1e735e8b8470d34b40e61d95`；[上游许可证](https://github.com/RigoLigoRLC/unixnu-nativeqcomtsens/blob/8b2fb5e610eaed8fb7c0dc368da9e379adc0e12f/LICENSE) 为 BSD-2-Clause-Patent，完整保留在本目录 `LICENSE`。

它提供了 XNU 映射、只读采样、使能检查、三次有效位尝试和状态输出的现成思路。但该固定版本的检查是：

```c
/* Existing reference; not the check used by this port: */
if (!(result.ready & 8)) { /* ... NotReady ... */ }
```

批注：这里没有照抄 bit 3。SC7180 对应的 Linux v2 字段定义为 bit 0；本移植使用 `CT_READY = 1`，模型用 `ready=8` 必须拒绝、`ready=1` 才通过来防止回归。另增加版本／reset 状态；IOKit C++ 映射改用当前内核已有的 `ml_io_map()`，仅在 sampler 单线程运行。

## 5. 不混用板级热敏电阻

[Coachz rev1/2 DTS](https://github.com/torvalds/linux/blob/36aad21d50d1af3f6954bb0c988257a908202477/arch/arm64/boot/dts/qcom/sc7180-trogdor-coachz-r1.dts) 对 47kΩ 板级热敏电阻有专门禁用说明。这里采集的是 SoC TSENS，不是电池、充电口或机壳 NTC；未判断具体机器板号，也没有把机壳热区阈值作为 CPU 结温阈值。

本地核对的 SHA-256：

| 固定源文件 | SHA-256 |
| --- | --- |
| sc7180.dtsi | `d15a49609b66d16b76df3728ea190b3ed2418723667179ff6514fddf4c26ef83` |
| tsens-v2.c | `0f0d41735b468d247f9d648980b31b30d91df4976d9f74c48767014c0ec74a12` |
| tsens.c | `9e444b34402c41da6e6037863dbce855f0cfb2e17785784df729f8078d75e624` |
| NativeQcomTsens.cpp | `65a8bbfc50e511e805e4a85bf068178f0f33ddb6c257e2451239f34240feb5c5` |

这些摘要用于固定调查版本，不代表机器上的固件或 TSENS 状态。
