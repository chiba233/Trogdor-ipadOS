# eMMC 与 microSD（sdhci-msm）

驱动已编进预链接内核（RamBlock 的归档、采样线程启动、`coachz-emmc=2 coachz-sd=2`）。真机上 eMMC 以 52 MHz、8 位总线识别，只向机主切出的 1 GiB 分区 `coachz-scratch` 写入，窗口外的写被拒绝，2048 块写后读回无错；跨重启保留和分区发布还没有证实；microSD 槽没插卡时报告无卡，读写卡未测。入口：[实现与接入说明](../../platform/coachz-prelink/drivers/RamBlock/mmc/README.md)，[固定源码](../../platform/coachz-prelink/drivers/RamBlock/mmc/SOURCES.md)。

## 路线

照主线 Linux 里这颗 SoC、这块板子用的那份驱动移植（Linux 7.4-rc1，`6628e7d4c6b2`）：`sdhci-msm` 的 sc7180 变体、SDHCI 核心、MMC/SD 初始化、`gcc-sc7180` 时钟、`qcom-rpmh-regulator` 供电、`rpmhpd` 的 CX 等级、trogdor/coachz 设备树。数值和顺序取自主线，代码自己写。Apple 没有 SDHCI 驱动；UniXNU 移植 wiki 的存储页只有 UFS，社区目录里的 NativeEmmc 面向 Redmi Note 9（merlinx），不是 sdhci-msm，也没有真机结果。块设备一层照 wiki 的 RamBlock 做法：`IOBlockStorageDevice` 加预链接 IOStorageFamily 绑定，先确认容量和读取。

## 第一阶段做什么

- 两个槽各自由启动参数打开：`coachz-emmc=1`（sdhc_1）、`coachz-sd=1`（sdhc_2）。
- 探测顺序照 `sdhci_msm_probe`：稳压器 HPM，OPP 投票，先 core 后 iface 时钟，vendor 寄存器复位值，能力位，先处理挂起的 pwr_irq，再接中断。
- 每次写 POWER_CONTROL、HOST_CONTROL2 或做 RESET_ALL，都按主线等 pwr_irq；中断线程经 RPMh 开关 vmmc/vqmmc，切 SDC 引脚状态，再写 `PWRCTL_CTL` 应答。
- eMMC：CMD1、CID、CSD、EXT_CSD，HS 52 MHz，8 位总线。SD：CMD8、ACMD41、SCR、CMD6 高速，4 位总线。
- 读：CMD17/CMD18，配合 Auto-CMD23 或 Auto-CMD12，ADMA2 读进固定的 64 KiB 缓冲。IOKit 发布只读块设备，GPT 由原版存储栈解析。

第一阶段没做的：HS200/HS400/UHS-I/调谐/DLL 校准、CQE、带宽投票、SD 热插拔。EXT_CSD 只改读数据必需、断电即失的 BUS_WIDTH 和 HS_TIMING；如果 PART_CONFIG 选的是启动分区，就只报告，不改写，因为改它会连带重写持久的启动位。

写入（`coachz-emmc=2`）：eMMC 上还有 Debian 的根和启动分区，所以块级写入只接受 GPT 名字和起止都对得上的 `coachz-scratch`，发命令之前先查窗口，其他 LBA 一律拒绝并计数。扩大范围要机主重新同意。microSD（`coachz-sd=2`）整张卡可写。

## 主机验证

`python3 -B -m unittest discover -s tests -p test_mmc.py -v`，3 项通过：

- 可移植层 10 组模型，ASan/UBSan 下通过。期望值都从主线公式、设备树或公开卡数据算出。
- 设备树里的 `sdc1_on`/`sdc2_on` 算出 `0x9FFF`/`0x1FE4`，和 coreboot 实际写入的值一致。
- `coachz_sdcc.c` 用桩头文件做 `-Wall -Wextra -Werror` 类型检查，通过。
- 确认当前构建没有引用这些代码。

没有编译内核或内核对象，没有打包、写盘，也没有访问设备。

## 接入方式

1. **共用 RPMh 客户端**：`coachz_rpmh_iface.h` 的四个函数实现在 Wi-Fi 补丁里，所有驱动共用同一个 TCS。CX 按使用者（SDCC1、SDCC2、调制解调器的代理票）取最大的 corner 再写，调制解调器 handover 时只撤自己那份，不再把 CX 投回 0。
2. RamBlock 的构建脚本把 `coachz_sdcc.c` 编进 `libCoachzUsb.a`，把 `CoachzMmcBlock.cpp` 用 NativeRamBlock 的同一条命令编进 `libRamBlock.a`；`coachz-prelink-mmc.patch` 在采样线程里紧挨着 Wi-Fi 启动它。
3. DMA 池 `0x81100000` 起 192 KiB，夹在 USB 池（`0x81000000`，1 MiB）和扬声器环（`0x81200000`）之间，不在任何保留区里。ArmCpu 的 SPI 槽位从 16 调到 32（触摸 1、Wi-Fi 的 CE 12、这两个核 4）。
4. 上机看 `COACHZ mmc` 日志：能否读到主机寄存器，pwr_irq 是否到来，卡能否识别，IOKit 能否发布 eMMC 的 GPT。

## 下一阶段（要等第一阶段的实机结果）

eMMC HS400ES 384 MHz（GPLL6、CX nom、DLL 校准、增强选通），SD UHS-I（CMD11、GPLL7、调谐），带宽投票，CQE，SD 热插拔。
