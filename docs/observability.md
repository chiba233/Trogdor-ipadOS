# 原生探针可读输出：接入调查

更新：2026-10-27。单指令匹配对照已支持 EL1 诊断路径的间接执行证据；下一步需要读取实际寄存器和设备树交接，避免继续只靠复位行为推断。

## 直接恢复原厂显示

锁定的 Depthcharge 分支在 handoff 前关闭三项状态：EC 背光 PWM、板级背光 GPIO，以及 MDSS timing engine／DSI controller。仅向 framebuffer 写字节不能显示内容。

- [Depthcharge 显示清理](https://chromium.googlesource.com/chromiumos/platform/depthcharge/+/719118f5e36ea312c33b8f005798b1488f9ec2d1/src/drivers/video/display.c)
- [SC7180 stop/init](https://chromium.googlesource.com/chromiumos/platform/depthcharge/+/719118f5e36ea312c33b8f005798b1488f9ec2d1/src/drivers/video/sc7180.c)
- [EC 背光 PWM](https://chromium.googlesource.com/chromiumos/platform/depthcharge/+/719118f5e36ea312c33b8f005798b1488f9ec2d1/src/drivers/video/ec_pwm_backlight.c)

Coreboot 的 DSI video-mode 配置设置控制器使能、四条数据 lane 和 clock lane；MDP 配置另有 flush。恢复是否只需重建被 stop 清除的状态，取决于寄存器、时钟、桥接器和面板是否仍保留初始化结果，尚不能由源码反推出硬件一定接受的恢复顺序。

- [Coreboot DSI 初始化](https://chromium.googlesource.com/chromiumos/third_party/coreboot/+/29f90de5f72c645a00146e04789896766f28e783/src/soc/qualcomm/sc7180/display/dsi.c)
- [Coreboot MDP 配置](https://chromium.googlesource.com/chromiumos/third_party/coreboot/+/29f90de5f72c645a00146e04789896766f28e783/src/soc/qualcomm/sc7180/display/mdss.c)

显示方案需要有界读取实际 framebuffer 与格式信息、校验地址／大小、恢复显示链和背光，再输出探针日志。不能硬编码其他设备的 framebuffer，也不能将运行中的 Linux 显示地址直接当作下一次固件交接地址。当前仅调查，没有加入显示或 EC 写入。

## 原生 Linux 接收诊断日志

另一条诊断路线是在同一引导期间，先执行独立探针，再交接已知可用的原生 Linux；探针把日志写入预留的 FDT 属性，Linux 启动后读取。它不需要虚拟机，也不作为 iPadOS 产品路径，但会增加 Linux payload、完整 DTB 和 initrd 的打包与交接工作。当前最小探针 DTB 不足以启动 Linux。

Linux ARM64 ABI 接受 NS EL1 或 EL2 入口，要求正确的 Image 位置、FDT、缓存／MMU、DAIF、timer 和 GIC 状态。现有探针的最小 EL1 初始化没有证明满足完整 Linux 启动契约，不能直接追加一次跳转就宣称可用。

- [Linux ARM64 引导契约](https://github.com/torvalds/linux/blob/v6.12/Documentation/arch/arm64/booting.rst)

该方案已选定并实现为 [EL1 探针→受控 HVC→EL2 Linux 的日志接收路径](linux-log.md)：让 Linux 保持从原厂 EL2 入口启动，恢复探针修改的 EL2 状态。Image、FDT、initrd、命令行和签名的主机校验及 29 项回归通过；硬件启动仍待验证。

## 复位后读取 CBMEM 的限制

Coreboot console 在有效 CBMEM entry 被保留时可继续追加旧内容，但这依赖底层内存与 CBMEM 生命周期。源码明确是条件性保留；不能把它当作本平台跨系统复位必定持久化的日志。当前可读取的固件日志没有此前探针记录。

- [Coreboot CBMEM console](https://chromium.googlesource.com/chromiumos/third_party/coreboot/+/29f90de5f72c645a00146e04789896766f28e783/src/lib/cbmem_console.c)

无论选择哪条路线，都要先取得 current EL、incoming EL、stage、CNTFRQ、FDT 范围；异常时还需 ESR／ELR／FAR，再开展 XNU 装载与 ADT 接入。这里记录的是候选方案与约束，不是已实现的日志通道。
