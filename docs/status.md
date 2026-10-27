# 状态与首阶段验收

更新：2026-10-27。

| 项目 | 状态 | 证据／限制 |
| --- | --- | --- |
| 原厂 Coreboot / BL31 / Depthcharge 调查 | 源码已核对 | 固定 Trogdor 维护分支，见 upstream.lock.json |
| NS EL2 入口与显示停止行为 | 入口已真机确认；显示停止为源码证据 | 日志 entry EL=2；显示寄存器状态未逐项测量 |
| ARM64 内存日志探针 | EL1 诊断已真机验收 | Linux 接收版本读回 entry EL=2、current EL=1、stage=3、handoff EL=2 |
| 可选 GENI UART 探针 | 已交叉编译 | 仅复用已初始化 UART；未真机启动 |
| Image 头、ELF 符号与 FIT 内嵌数据 | 主机检查通过 | build/manifest.json；不能证明 CPU 指令在硬件执行成功 |
| 启动期生成码 | 主机回归通过 | 显式关闭 FP/NEON、启用严格对齐；ELF 指令范围检查通过；匹配对照提供间接证据，后续 Linux 接收版本已取得直接日志 |
| 首次 EL2 USB 尝试 | 未通过 | 黑屏，未观察到预期复位；尚不能确认入口或停止位置 |
| FDT 解析与原厂 END token 格式兼容 | 主机测试通过 | tests/test_artifacts.py；包含截断和非法边界 |
| vboot 签名与容器验证 | 主机验证通过 | 使用锁定 vboot 分支构建的 futility / 公开测试密钥；两个检查点容器签名与 body 验证通过，篡改被拒绝；Linux 接收版本已读取入口寄存器并完成原生交接 |
| 无串口 EL2 / EL1 复位检查点 | EL1 诊断路径有间接硬件证据 | 原版重启，最终 SMC 改为 NOP 的匹配对照不重启；支持此前 EL1／stage／FDT 门限；该复位对照没有直接日志或精确计时 |
| GPT 镜像生成 | 主机测试通过 | 主／备 GPT CRC、分区类型、负载布局、文件保护；官方 vboot 对 64 MiB 镜像与模拟 8 GiB 介质的主表校验通过；Linux 接收版本的 128 MiB 镜像已通过完整介质回读与实际启动 |
| 仅抑制最终复位的 EL1 匹配对照 | 已观察到不自动重启 | Image 仅改一条 SMC 为 NOP，其余字节相同；与原版形成硬件对照，见 docs/control.md |
| 原生 Linux 日志接收 | 真机验收通过 | EL1 探针→受控 HVC→EL2 Linux；8192 字节日志完整读回并校验，29 项主机测试通过，见 docs/linux-log.md |
| 显示恢复 | 未实现 | 原厂停止 timing engine / DSI，不能直接沿用 splash |
| XNU 源码与装载前检查 | 源码内核已编译链接，51 项回归通过 | XNU 10063.141.1／Darwin 23.6；真实 FDT 已生成最小 ADT；本地链接指针已逐条审计；完整组件 prelink 待接入，见 docs/xnu-preparation.md |
| 源码 XNU 装载与 boot_args 入口检查点 | 真机验收通过 | EL1 读取参数后 HVC 返回 EL2，Linux 收到唯一且通过校验的 copied／checkpoint／boot_args／current EL 四字段，见 docs/xnu-checkpoint.md |
| XNU MMU 开启检查点 | 真机验收通过 | 完整 8192 字节日志确认 MMU／表池／硬件 AT 校验均成功，并返回 Linux；见 docs/xnu-mmu-checkpoint.md |
| XNU 高虚拟地址／C 入口栈读取检查点 | 真机验收通过 | 原始 RET 与栈帧调整执行，高地址／栈读取／C 入口三个字段为 1；完整 8192 字节日志通过校验，见 docs/xnu-highva-checkpoint.md |
| XNU 早期 C 正文／参数复制检查点 | 真机验收通过 | 完整日志的 C 正文／栈写入／参数复制／缓存维护四字段及原有 11 字段通过，完整父子帧与参数内容核对后返回 Linux；见 docs/xnu-cinit-checkpoint.md |
| XNU 启动 CPU 数据初始化检查点 | 真机验收通过 | 完整 16 字段通过；原始标量／FP/NEON 写入后的 CPU 对象、保留哨兵和 guard 全部核对，并返回 Linux；见 docs/xnu-cpu-checkpoint.md |
| XNU 平台入口与首次原生 ADT 查找 | 91 项主机回归与独立审查通过，真机待验收 | 正常 CPU 初值，原始平台状态初始化与 arm-io 查找后返回；外置签名镜像已全量回读，见 docs/xnu-platform-checkpoint.md |
| XNU 后续平台／中断／SMP 初始化 | 未验证 | 本轮仅到首次 ADT 查找返回，后续 SoC 属性、设备和中断初始化尚未进入 |
| eMMC、触摸、GPU 与完整 iPadOS 用户态 | 未实现 | 没有原生桌面或应用运行记录 |

## 已执行的主机验证

- Zig 0.15.2 的后端特性覆盖问题已定位，详见 docs/codegen.md；原有 17 项主机测试通过，四种 ELF 的指令范围通过标量检查。
- Zig 0.15.2 构建 memory / uart / reset-el2 / reset-el1 四种 AArch64 freestanding ELF。
- 从 PT_LOAD 段导出原始 Image，校验 ARM64 magic、跳转、内存大小和 ELF 符号。
- 生成并核对 FIT 的 Image / DTB 原始字节与 SHA-256。
- 运行 FDT 和产物测试；本地原厂参考源码的 HEAD 与锁定版本一致。
- 两个检查点已实际签名；核对签名容器／GPT 中的 FIT 原始字节一致，篡改 body 会被 futility 拒绝。
- 定时复位条件拒绝错误 EL、未完成阶段、不可用 PSCI 和异常计时频率；GPT 格式与仅文件输出限制通过测试。
- 独立源码审查发现原厂 dt_flatten() 的 structure_size 不计 FDT_END；已增加精确边界兼容与回归用例。

匹配对照增加 4 项回归，合计 21 项通过。实际产物检查确认除最终复位的四个指令字节外 Image 完全相同，FIT 只改变 kernel data 与其哈希。

## 硬件尝试与待完成验收

已记录的尝试见 [硬件记录](hardware-tests.md)。仅凭最初两轮自动重启不能确定执行位置；后续单指令匹配对照不再自动重启，使最终 SYSTEM_RESET 及此前 EL1／诊断门限获得较强的间接硬件支持。该对照本身不是直接寄存器测量；2026-10-27 后续 Linux 接收版本已取得完整探针日志，直接确认 entry EL2、current EL1、stage=3、handoff EL2。首阶段原生探针验收通过，见 [硬件记录](hardware-tests.md)。

首阶段验收记录如下：

1. 已验证签名、body、GPT 和全部 128 MiB 介质回读 SHA-256。
2. 已通过同次引导的 Linux 接收路径读取日志；不依赖跨复位内存保留，AP UART 未验证。
3. 已从单独准备的可启动介质，经原厂开发者模式进入探针并交接原生 Linux。
4. 已读取入口 EL、当前 EL、MIDR / MPIDR / CNTFRQ、FDT 内存与 Coreboot 表属性；原始地址与板级数据私下保存。
5. 已确认 current EL = 1 且 stage = 3，handoff EL = 2；本次正常路径未触发异常，ESR / ELR / FAR 异常接收仍待单独验证。
6. 后续已完成：源码 XNU 的运行时段装载与 pre-MMU boot_args 读取检查点，取得有效硬件日志。随后 MMU 开启、完整页表池和硬件 AT 检查点也已通过真机验收。随后高虚拟地址取指、C 入口栈帧调整与诊断栈读取也已取得硬件日志。早期 C 正文的原始栈保存、参数复制和缓存清理返回也已取得硬件日志。启动 CPU 对象的原始标量／SIMD 初始化也已通过硬件日志验收。待完成：后续平台／中断／SMP 初始化、ADT 消费、平台接口与显示恢复。

原生探针、源码 XNU pre-MMU 入口、MMU 开启、高地址入口栈读取、早期 C 参数复制与启动 CPU 数据检查点均已取得硬件日志；主机测试仍须单独列出。没有完整 XNU 或 iPadOS 启动结果。
