# 电池与扬声器：已接入构建

两份 XNU 补丁都已加进 `series`，启动参数也已打开。真机结果在 `docs/hardware-tests.md`。

扬声器的 DMA 缓冲后来改到了内核窗口外的低端 DRAM（`0x81200000`），因为内核内存全在 4 GiB 以上。

## 补丁

| 补丁 | 内容 | 启动参数 |
|---|---|---|
| `coachz-prelink-battery.patch` | `bsd/dev/coachz_ec.c`、`coachz_ec_proto.h`：在 QUP1 SE0（`0x00a80000`）的 GENI SPI 上跑 Chrome EC protocol 3，一把锁串行所有事务；`iokit/Kernel/CoachzBattery.cpp`：每 10 秒读 `BATTERY_GET_DYNAMIC` 和 `CHARGE_STATE`，作为 `IOPMPowerSource` 发布 | `coachz-battery=1` |
| `coachz-prelink-speaker.patch` | `iokit/Kernel/CoachzSpeaker.cpp`：开机后在板载扬声器上播一次 1 秒、440 Hz、约 −18 dBFS 的正弦波 | `coachz-speaker=1` |

两份补丁都在 `bsd/dev/coachz_sample.c` 的采样线程开头加一行启动调用，并在 `bsd/conf/files` / `iokit/conf/files` 登记新文件。**扬声器补丁的上下文以电池补丁为前提**：要么只加电池，要么按电池、扬声器的顺序都加；单独加扬声器打不上。

接入只需三步：

1. 在 `series` 末尾按顺序加：
   ```
   # Chrome EC battery: protocol-3 queries over QUP1 SE0 GENI SPI, published as an IOPMPowerSource.
   coachz-prelink-battery.patch

   # A one-second 440 Hz speaker tone through LPASS secondary MI2S, once after boot.
   coachz-prelink-speaker.patch
   ```
2. 在 `tools/xnu_screen.py` 显示路线的命令行加 ` coachz-battery=1`、` coachz-speaker=1`（可以只加一个）。
3. 校验脚本可检查内核里有 `COACHZ battery: %d %%` 和 `COACHZ speaker: tone %s` 两个字符串。

## 依据

- **电池**：Chrome EC 自己负责充电器、PD 和电池；本补丁只发查询命令，不发 `CHARGE_CONTROL`、`CHARGE_CURRENT_LIMIT`、PD 角色或任何写配置的命令。总线代码来自 `boot/display_ec.c`，它已在本机向同一 EC 发过 HELLO 和背光命令，这里放大了缓冲区、加了 command version 和锁。时钟投票：GCC `0x52008` 的 wrap1 core_2x/core/m_ahb/s_ahb 与 SE0（18～22 位），SE0 分支 `0x18014`（Coreboot sc7180）。命令结构来自固定版本的 Chrome EC `include/ec_commands.h`。发布方式参照 UniXNU-sheng 的 `ShengBattery.cpp`：`CurrentCapacity` 为百分比、`MaxCapacity` 为 100，真实 mAh 放在 `AppleRawCurrentCapacity`/`AppleRawMaxCapacity`，第一次读到有效值后才 `registerService()`；读取失败时保留上一次的好值，不会变成 0%。电流按 EC 原样发布（mA，负数为放电），不取负。
- **扬声器**：寄存器地址、数值和顺序是工厂 Depthcharge `drivers/bus/i2s/sc7180.c`（`new_sc7180_i2s(48000, 2, 16, LPASS_SECONDARY, 0x62000000)`）的硬件事实，代码按 BSD 重写；LPAIF 的 I2S、IRQ、RDMA 地址与 Linux `lpass-sc7180` 一致。功放使能是 TLMM SOUTH 块的 GPIO23（Coreboot 开机时设为低）；MI2S 引脚 GPIO49/50/51 由 Coreboot 复用为 MI2S_1。DMA 缓冲区是低于 4 GiB 的物理连续内存，依赖固件留下的 SMMU 直通（USB 驱动的 DMA 在同样前提下已工作）。每一步有超时，失败即停；结束时先关输出、停 DMA、关功放，再释放缓冲区。

## 真机上要看什么

日志都会进外接盘末尾的内核日志环，回到 Debian 后用 `sudo -n /usr/local/sbin/coachz-read-log` 读取。

**电池**

1. 应出现 `COACHZ battery: <百分比> % <mV> mV <mA> mA <剩余>/<满充> mAh flags 0x… ac … (read 1, EC state 0)`，随后 `COACHZ battery: published`。
2. 数值与 Debian 下 `/sys/class/power_supply/sbs-12-000b/`（`voltage_now`、`current_now`、`charge_now`、`charge_full`、`capacity`）和 `CROS_USBPD_CHARGER*/online` 对得上，单位换算后相符。
3. 状态栏电池图标显示同样的百分比；插拔充电器后 `flags` 和充电状态跟着变（数值变化时才打印新的一行）。
4. 若出现 `read N failed: error 0x…`：低字节 1～7 是协议错误（`coachz_ec_proto.h`），`0x10` 以上是传输错误（`coachz_ec.c`），`EC_ERR_RESULT`（7）的高位是 EC 返回码。

**扬声器**

1. 开机后约十几秒（采样线程启动时）听到一秒的 440 Hz 音。
2. 日志 `COACHZ speaker: tone played: 2 periods, 0 errors, irq 0x…, ~1000 ms, DMA at 0x…`。
3. 失败时会停在对应一步并打印：`GDSC … did not power on`、`audio PLL did not lock`、`no contiguous DMA buffer` 或 `tone incomplete`。
4. 播放结束后显示、触摸和外接盘都不受影响。

## 还没做的

- 电池：温度、循环次数、设计容量（需要 `BATTERY_GET_STATIC` 和经 EC 的 SBS 读寄存器）、适配器详情（`USB_PD_POWER_INFO`）、剩余时间、EC 中断、低电量优雅关机、休眠。
- 扬声器：持续播放（环形缓冲、周期中断、欠载）、AudioServerPlugIn 与 audioserver 接入、音量。参考 UniXNU-sheng 的 `ShengAudioService.cpp` / `ShengAudioPlugIn.c`。
- 机身按键和键盘见 `docs/plans/input.md`，屏幕背光见 `docs/plans/backlight.md`（同样已写好、未接入构建）。
- Wi-Fi（WCN3990，参考 ios-flame）和 GPU（A618，参考 UniXNU Adreno/AdrenoMetal 与 sweet 的 legacy HFI）尚未开始写代码。
