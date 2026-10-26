# Coachz 原厂启动链：源码调查

调查日期：2026-10-26。源码版本由 `upstream.lock.json` 固定。维护分支源码不能证明任意设备当前安装的二进制固件版本；以下为源码调查；2026-10-27 的探针日志已直接确认原厂 EL2 入口和探针 EL1 执行，见 [硬件记录](hardware-tests.md)。其余源码结论不自动视为逐项硬件验收。

## 原厂平台与执行级别

Coachz 由 `BOARD_GOOGLE_TROGDOR_COMMON` 选择 `SOC_QUALCOMM_SC7180`。该 SoC 配置选择 `ARM64_USE_ARM_TRUSTED_FIRMWARE`；构建把 Coreboot bootblock 与 Qualcomm 安全部件封装为启动镜像，不能照搬 Android ABL 的交接假设。

- [Coreboot SC7180 Kconfig](https://chromium.googlesource.com/chromiumos/third_party/coreboot/+/29f90de5f72c645a00146e04789896766f28e783/src/soc/qualcomm/sc7180/Kconfig)
- [SC7180 构建规则](https://chromium.googlesource.com/chromiumos/third_party/coreboot/+/29f90de5f72c645a00146e04789896766f28e783/src/soc/qualcomm/sc7180/Makefile.inc)

`src/arch/arm64/boot.c:run_payload()` 用 `get_eret_el(EL2, SPSR_USE_L)` 调用 `run_bl31()`；`bl31.c` 把这一 SPSR 和 Depthcharge 入口放入 BL33 信息。libpayload 的入口直接配置 SCTLR_EL2，也没有降到 EL1。

- [Coreboot payload EL2](https://chromium.googlesource.com/chromiumos/third_party/coreboot/+/29f90de5f72c645a00146e04789896766f28e783/src/arch/arm64/boot.c#10)
- [BL31 交接](https://chromium.googlesource.com/chromiumos/third_party/coreboot/+/29f90de5f72c645a00146e04789896766f28e783/src/arch/arm64/bl31.c)
- [libpayload EL2 入口](https://chromium.googlesource.com/chromiumos/third_party/coreboot/+/29f90de5f72c645a00146e04789896766f28e783/payloads/libpayload/arch/arm64/head.S)

检查的 DarwinBoot 则要求 `DarwinCurrentEl() == 4`（即 EL1），否则返回 EFI_UNSUPPORTED。它的 Entry.S 也明确是 EL1-only。因此“U-Boot 能执行 EFI”不足以证明这一版 DarwinBoot 可以直接启动。

- [DarwinBoot EL 检查](https://github.com/UniXNU/DarwinBoot/blob/3325e352d7df68a9f9ad74832ed94a6acbbd7390/DarwinBootPkg/Driver/DarwinBoot.c#L506)
- [DarwinBoot 入口汇编](https://github.com/UniXNU/DarwinBoot/blob/3325e352d7df68a9f9ad74832ed94a6acbbd7390/DarwinBootPkg/Driver/Entry.S)

## 原厂内核入口

Trogdor 的 Depthcharge 配置启用 FIT，设定：

| 用途 | 配置值 |
| --- | --- |
| FIT / 内核容器加载区 | 0xD0000000 |
| FDT 放置区 | 0xF0000000 |
| Depthcharge | 0xF1000000 |
| 默认容器加载区大小 | 0x20000000（512 MiB） |

这些不是 XNU 可以直接占用的地址。`boot64.c` 会读取 ARM64 Image 头，检查 magic `0x644d5241`，再依据 Coreboot RAM 表、2 MiB 对齐、text_offset 和 image_size 选择解压位置，并拒绝与上面的区域重叠。最后刷新缓存、关闭 MMU，以 `handoff(fdt, 0, 0, 0)` 调用入口；没有 ERET 到 EL1。

- [原厂 Trogdor 配置](https://chromium.googlesource.com/chromiumos/platform/depthcharge/+/719118f5e36ea312c33b8f005798b1488f9ec2d1/board/trogdor/defconfig)
- [内核容器大小默认值](https://chromium.googlesource.com/chromiumos/platform/depthcharge/+/719118f5e36ea312c33b8f005798b1488f9ec2d1/src/image/Kconfig)
- [ARM64 解压、重定位与交接](https://chromium.googlesource.com/chromiumos/platform/depthcharge/+/719118f5e36ea312c33b8f005798b1488f9ec2d1/src/arch/arm/boot64.c)

据此，可以研究带 Linux Image 头的自写 shim；这并不要求最终执行的是 Linux。先验证探针，再加入 Mach-O 装载、XNU boot_args 和 ADT。现阶段没有验证过的 Coachz XNU 镜像。

## FDT 与内存

FIT 配置依据原厂 mainboard 名称、board ID、SKU 选择。Depthcharge 会更新 `/chosen`、内存表和保留区，设置 initrd 地址，并在 `/firmware/coreboot` 中公开 Coreboot table / CBMEM 地址和板级标识。

原厂分支的 initrd 属性使用 32-bit 地址，代码明确假定 initrd 位于 4 GiB 以下。首阶段不携带大型用户态 RAM 盘，避免把未来根文件系统设计建立在错误的加载限制上。

- [FIT 选择、内存更新及 initrd](https://chromium.googlesource.com/chromiumos/platform/depthcharge/+/719118f5e36ea312c33b8f005798b1488f9ec2d1/src/boot/fit.c)
- [Coreboot 表传递](https://chromium.googlesource.com/chromiumos/platform/depthcharge/+/719118f5e36ea312c33b8f005798b1488f9ec2d1/src/boot/coreboot.c)

## 原厂 FDT 序列化细节

该分支的 dt_flatten() 把 FDT_END 写在 size_dt_struct 覆盖区之后，strings_offset 刚好在该 token 后面。探针只在完整 blob 范围内、且严格满足这一布局时兼容；不放宽普通越界检查。该行为已有回归用例。

- [原厂 dt_flatten](https://chromium.googlesource.com/chromiumos/platform/depthcharge/+/719118f5e36ea312c33b8f005798b1488f9ec2d1/src/base/device_tree.c#469)

## 显示：不能假设 continuous splash

Coreboot ramstage 已包含 SC7180 DSI、TI SN65DSI86、EDID 和面板初始化。libpayload 可动态分配实际 framebuffer，Depthcharge 把它写入显示地址寄存器。因此不能硬编码来自 Android 机型的 splash 地址。

更关键的是 `arch/arm/fit.c` 在启动前调用 `run_cleanup_funcs(CleanupOnHandoff)`。原厂 `display_cleanup()` 会关背光并调用 `sc7180_display_stop()`，后者：

1. 写 timing engine 0x0AE6B800 = 0。
2. 等待约 20 ms。
3. 写 DSI control 0x0AE94004 = 0。

原厂维护分支对 Legacy 交接同样执行 stop；当前主线对 Legacy 的处理已有变化，不能混用。恢复显示需要重建被关闭的状态，仅向原 framebuffer 写像素不够。现有探针只记录，不写这些显示寄存器。

- [Coreboot 面板初始化](https://chromium.googlesource.com/chromiumos/third_party/coreboot/+/29f90de5f72c645a00146e04789896766f28e783/src/mainboard/google/trogdor/mainboard.c)
- [libpayload framebuffer 分配](https://chromium.googlesource.com/chromiumos/third_party/coreboot/+/29f90de5f72c645a00146e04789896766f28e783/payloads/libpayload/arch/arm64/mmu.c)
- [启动前清理](https://chromium.googlesource.com/chromiumos/platform/depthcharge/+/719118f5e36ea312c33b8f005798b1488f9ec2d1/src/arch/arm/fit.c)
- [显示清理回调](https://chromium.googlesource.com/chromiumos/platform/depthcharge/+/719118f5e36ea312c33b8f005798b1488f9ec2d1/src/drivers/video/display.c)
- [SC7180 显示停止](https://chromium.googlesource.com/chromiumos/platform/depthcharge/+/719118f5e36ea312c33b8f005798b1488f9ec2d1/src/drivers/video/sc7180.c)

## 首阶段串口与后续存储

SC7180 的原厂 console 使用 QUP instance 8，基地址 0x00A88000。原厂源码公开了 UART 协议判定、TX transaction length、master command 与 FIFO 的接口。探针只在已配置状态下发送；不能认为“加载了 QUP firmware”就等于已配置 UART 波特率。

- [原厂 UART](https://chromium.googlesource.com/chromiumos/third_party/coreboot/+/29f90de5f72c645a00146e04789896766f28e783/src/soc/qualcomm/sc7180/qupv3_uart.c)
- [寄存器布局](https://chromium.googlesource.com/chromiumos/third_party/coreboot/+/29f90de5f72c645a00146e04789896766f28e783/src/soc/qualcomm/sc7180/include/soc/qcom_qup_se.h)

原厂已有 eMMC / SD 初始化和读写实现，后续存储移植应先研究这些小型固件驱动，而非直接套用 Android UFS。Depthcharge 的驱动不是交接后可调用的 UEFI Block I/O 服务，仍需接入 XNU 的存储接口。

- [Trogdor 存储配置](https://chromium.googlesource.com/chromiumos/platform/depthcharge/+/719118f5e36ea312c33b8f005798b1488f9ec2d1/src/board/trogdor/board.c)
- [MSM SDHCI](https://chromium.googlesource.com/chromiumos/platform/depthcharge/+/719118f5e36ea312c33b8f005798b1488f9ec2d1/src/drivers/storage/sdhci_msm.c)

## 无串口复位观察

原厂 Trogdor 配置启用 `DRIVER_VIDEO_EC_PWM_BACKLIGHT`。显示清理还通过 EC 把 PWM 设为零，所以仅重新打开背光 GPIO 和 DSI 不保证有可见图像。

原厂 `drivers/power/psci.c` 使用 SMC `0x84000009` 请求系统复位。独立检查点复用这一 ABI，先检查 PSCI 版本，再用物理计数器延时。EL2 与 EL1 的不同延时用于区分两个执行路径；该通道不传递寄存器值，也不把固件错误返回菜单认作探针成功。

- [原厂 PWM 配置](https://chromium.googlesource.com/chromiumos/platform/depthcharge/+/719118f5e36ea312c33b8f005798b1488f9ec2d1/board/trogdor/defconfig)
- [原厂 EC 背光 PWM](https://chromium.googlesource.com/chromiumos/platform/depthcharge/+/719118f5e36ea312c33b8f005798b1488f9ec2d1/src/drivers/video/ec_pwm_backlight.c)
- [原厂 PSCI 复位](https://chromium.googlesource.com/chromiumos/platform/depthcharge/+/719118f5e36ea312c33b8f005798b1488f9ec2d1/src/drivers/power/psci.c)
