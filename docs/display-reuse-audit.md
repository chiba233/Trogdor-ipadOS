# 复用固件显示链：源码审计与独立验证范围

日期：2026-10-31。本文是对固件显示复用路线的源码核对，不表示显示恢复已实现或真机寄存器已测量，也不改变当前 version11 的 WFE／拓扑介质。优先研究 Coreboot／Depthcharge 的小型显示路径；本阶段不移植 Linux msm DRM 的完整初始化框架。

## 已核对的停机范围

固定 Depthcharge `719118f5e36ea312c33b8f005798b1488f9ec2d1` 的 `src/drivers/video/display.c:display_cleanup` 依次禁用图形缓冲、清黑、关闭背光、调用 display stop。`src/drivers/video/sc7180.c` 中的 stop 再关闭背光，写 `0x0AE6B800 = 0`，等 20 ms，写 `0x0AE94004 = 0`。这里没有关闭显示时钟、PHY／PLL、regulator，或发送面板 sleep DCS。

同分支的 display init 只把 libpayload 当前 framebuffer PA 写入 `0x0AE05014`，再置 timing engine 为 1。固定 Coreboot `29f90de5f72c645a00146e04789896766f28e783` 的 `src/soc/qualcomm/sc7180/display/dsi.c:mdss_dsi_video_mode_config` 为四 lane 构造的 control 是 `0x1f3`。这支持“保留整条固件链、只恢复停止的部分”作为首选实验假设；它还不能证明交接后其他 cleanup／EL3 行为及真实硬件保留状态，不能把两次写入直接称为显示已恢复。

公开来源：[显示清理](https://chromium.googlesource.com/chromiumos/platform/depthcharge/+/719118f5e36ea312c33b8f005798b1488f9ec2d1/src/drivers/video/display.c)、[SC7180 stop／init](https://chromium.googlesource.com/chromiumos/platform/depthcharge/+/719118f5e36ea312c33b8f005798b1488f9ec2d1/src/drivers/video/sc7180.c)、[DSI 配置](https://chromium.googlesource.com/chromiumos/third_party/coreboot/+/29f90de5f72c645a00146e04789896766f28e783/src/soc/qualcomm/sc7180/display/dsi.c)。这些是维护分支源码，不能等同于对当前安装固件每个二进制字节的匹配证明。

## framebuffer 发现仍缺一层

当前探针 `boot/probe.c` 只读取并记录 FDT 的 `/firmware/coreboot/reg`、board-id 和 sku-id，尚未解析 Coreboot 表头、校验和、转发表与 framebuffer／GPIO 项。

原始表中的地址也不能直接当作最终 scanout：libpayload 的 `libc/coreboot.c:cb_parse_framebuffer` 将表项按值复制进 `lib_sysinfo`；`arch/arm64/mmu.c:mmu_add_fb_range` 可以在 PA 为零时分配 framebuffer，只更新该副本；Depthcharge 从该副本编程 SSPP。因而应先取得校验后的几何／stride／bpp／mask／orientation，再与当次入口的 SSPP 源地址和 stride／format 核对。表项为空、PA 不一致、越界或重叠都应停止，不能猜一个 Android splash 地址或扫描任意物理 RAM。

内存占用按实际 `bytes_per_line × y_resolution`、页／分配对齐和显示扫描范围计算，检查溢出、RAM 归属与其他拥有者。不能只减掉固定的宽×高×4：规划器、自有装载区及以后 XNU VM 分配范围均须排除完整 framebuffer；原有 FDT reserved-memory／Coreboot 预留也要保留。Depthcharge 当前 FIT 内存更新从 lib_sysinfo 的 RAM 表生成，并不因为 framebuffer 副本存在就自动证明该分配已经进入传给内核的预留表。

Coreboot 当前板级显示路径设置 framebuffer 32 bpp，DSI 链路为 RGB888；两者不是同一属性。MDP 编程 stride、format `0x000236ff` 和 unpack `0x03020001`。实际尺寸、方向、通道顺序仍应由当次有效元数据与寄存器一致性核实，不能固定为 2160×1440。显示 DMA 与 CPU 的缓存一致性也需要显式处理；沿用原 PA 可以减少未知因素，却不等于已经证明 SMMU 的全部配置或旁路状态。

来源：[framebuffer 按值复制](https://chromium.googlesource.com/chromiumos/third_party/coreboot/+/29f90de5f72c645a00146e04789896766f28e783/payloads/libpayload/libc/coreboot.c)、[libpayload 动态分配](https://chromium.googlesource.com/chromiumos/third_party/coreboot/+/29f90de5f72c645a00146e04789896766f28e783/payloads/libpayload/arch/arm64/mmu.c)、[MDP 源配置](https://chromium.googlesource.com/chromiumos/third_party/coreboot/+/29f90de5f72c645a00146e04789896766f28e783/src/soc/qualcomm/sc7180/display/mdss.c)、[FIT 内存更新](https://chromium.googlesource.com/chromiumos/platform/depthcharge/+/719118f5e36ea312c33b8f005798b1488f9ec2d1/src/boot/fit.c)。

## 背光接口已经定位

固定 Depthcharge 的 `ec_pwm_backlight.c` 默认亮度为 80%；`ec.c:cros_ec_set_bl_pwm_duty` 发送 `EC_CMD_PWM_SET_DUTY`、命令版本 0。固定 Coreboot 的 `src/ec/google/chromeec/ec_commands.h` 定义命令号 `0x0025`，参数为小端 u16 duty、u8 pwm_type、u8 index；display-light 类型是 2，index=0，满量程 65535，80% 对应 52428。GPIO 来自名称为 backlight 的 Coreboot 项，不能硬编码板级 GPIO 或忽略极性。

板级 `board.c` 在 Coachz 路径使用 QUP1 SE0（`0x00a80000`）作为 EC SPI；只有早期 Trogdor rev0 存在与 TPM 总线交换的特殊分支。必须确认板型和实际入口控制器协议，不能误用 SPI NOR 控制器。Depthcharge 的 QUP SPI 路径在每次传输设置 FIFO packing、CS0、8-bit 字长和 fragment 操作；“固件初始化过 SPI”不等于裸机只发一帧字节就能工作。

协议 3 请求头 8 字节，含结构版本 3、checksum、u16 command、command_version、reserved、u16 data_len；整包字节和为零。EC SPI 保持 CS，发请求后轮询 `FRAME_START=0xec`，再接收 8 字节响应头及有界数据，核对版本、长度、reserved、result 和 checksum。固定实现要求事务间 CS 释放至少 200 µs、配置的唤醒延迟、接受／处理超时以及错误时释放 CS。首个通信实验应先做只读命令及结果校验，再考虑背光 duty 设置与结束恢复；本文没有执行 EC 命令或授权新显示写入器。

来源：[背光默认值](https://chromium.googlesource.com/chromiumos/platform/depthcharge/+/719118f5e36ea312c33b8f005798b1488f9ec2d1/src/drivers/video/ec_pwm_backlight.c)、[EC 命令调用](https://chromium.googlesource.com/chromiumos/platform/depthcharge/+/719118f5e36ea312c33b8f005798b1488f9ec2d1/src/drivers/ec/cros/ec.c)、[固定命令定义](https://chromium.googlesource.com/chromiumos/third_party/coreboot/+/29f90de5f72c645a00146e04789896766f28e783/src/ec/google/chromeec/ec_commands.h)、[SPI 协议](https://chromium.googlesource.com/chromiumos/platform/depthcharge/+/719118f5e36ea312c33b8f005798b1488f9ec2d1/src/drivers/ec/cros/spi.c)、[板级总线选择](https://chromium.googlesource.com/chromiumos/platform/depthcharge/+/719118f5e36ea312c33b8f005798b1488f9ec2d1/src/board/trogdor/board.c)。

## XNU 控制台可复用，但不是当前阶段自动可用

固定源码内核的 `pexpert/arm/pe_init.c` 会把 boot_args.Video 复制到 PE video 状态，保留 packed depth 中的旋转／缩放字段，并设置像素格式。当前检查点输入的 Video 为零；不能直接改变它而继续沿用已有 PE 状态的逐字节预期。

当前 `arm_init` 调用顺序在 `arm_vm_init` 之后才创建 console。`video_console.c:initialize_screen` 还依据 kernel_map 与 baseAddr 低位区分物理／虚拟地址，并通过 `ml_io_map_unmappable` 建立映射；需要正确的物理地址标记、内存类型及完整映射范围。`-v` 只是输出策略的一部分，不能跳过这些条件。通用函数虽有早期映射分支，也不能据此把当前版本宣称为支持 VM 前 video console。

“kprintf 只走串口”对该源码并不完全成立：`pexpert/arm/pe_kprintf.c:PE_init_kprintf` 在 `serial_init()` 失败时选择 `console_write_unbuffered`。DEVELOPMENT 构建还受 `disable_serial_output`／`disable_kprintf_output`、debug 位与 `-enable_kprintf_spam` 控制；当前 `-v debug=0xc ...` 不能当作已开启 kprintf。先审计并使用现有 console fallback，再决定是否有必要注册 framebuffer 串口后端。panic 还有独立的同步、锁和输出可用性条件，不能承诺所有 panic 自动上屏。

`pe_kprintf.c` 和 `video_console.c` 与锁定 Apple XNU 文件逐字节相同：[kprintf 配置与 fallback](https://github.com/apple-oss-distributions/xnu/blob/d8b80295118ef25ac3a784134bcf95cd8e88109f/pexpert/arm/pe_kprintf.c)、[video console 映射与绘制](https://github.com/apple-oss-distributions/xnu/blob/d8b80295118ef25ac3a784134bcf95cd8e88109f/osfmk/console/video_console.c)。PE／启动次序以当前实际移植源码为准，而非仅看上游通用函数。

## 建议的单独实验边界

1. 先完成表解析与入口快照，只读核对 framebuffer 元数据、scanout、TE／DSI 控制值、GPIO 和 EC 总线身份，形成有界输入；不从运行中的 Linux DRM 寄存器反推固件交接状态。
2. 独立显示探针沿已验收的 EL1／HVC／Linux 日志返回路径运行，不叠加新的 XNU 执行阶段。先在验证过的原 framebuffer 画带序号的图案，处理缓存，再按审计顺序恢复显示，限时观察。可以用不打开背光的手电照屏作为辅助现象，不能把“没看见”判定为 TE／DSI 必然失败。
3. EC 先只读通信，再单独验证 GPIO／PWM 恢复；超时有界，退出释放 CS，并在 Linux 交接前恢复可恢复的入口显示／背光状态。屏幕现象与同次引导日志分别验收，任何失败不得冒充 XNU 检查点通过。
4. 确认图案可见后，再加最小字符绘制，供 EL2 已捕获的故障使用。绘制不得分配内存、取锁、调用 kprintf 或访问未经验证的 framebuffer；坏显示描述／显示自身异常仍须保留原日志路径。version11 保留原生 EL1 向量，不能假定所有异常都已能 HVC 回收。
5. 独立探针验收后，才接 boot_args.Video、XNU VM 映射／排除和现有 console fallback，并为非零 Video 更新状态模型与测试。

本文未新建显示运行时代码或显示测试介质。当前先完成并验收 version11 的拓扑边界；显示路线可据此单独设计和实现。
