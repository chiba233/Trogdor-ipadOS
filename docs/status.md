# 状态与首阶段验收

更新：2026-10-31。

| 项目 | 状态 | 证据／限制 |
| --- | --- | --- |
| 原厂 Coreboot / BL31 / Depthcharge 调查 | 源码已核对 | 固定 Trogdor 维护分支，见 upstream.lock.json |
| NS EL2 入口与显示停止行为 | 入口、TE／DSI 关闭值已真机确认 | 日志 entry EL=2；独立发现中 TE／DSI 均为零；其余显示状态未完整验收 |
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
| 原厂显示交接只读发现 | schema 2 真机严格 45 字段通过，候选为真 | 13 次读取、零异常／写入并交接 Linux；元数据、分配关系与显示归属均成立。此前 schema 1 候选为假的原因已查明并修正。不表示画图或背光支持，见 [契约](display-discovery.md) |
| 显示恢复 | 背光实验真机通过：不用手电可见 8×6 棋盘格，EC 命令与背光 GPIO 正常，64 字段通过；此前无背光实验手电下可见，帧计数 60.00 Hz，58 字段通过 | stop 只关背光／TE／DSI control；实际 scanout 地址、交接保留状态、EC 协议和独立测试范围见 [显示审计](display-reuse-audit.md) |
| XNU 源码与装载前检查 | 源码内核已编译链接，51 项回归通过 | XNU 10063.141.1／Darwin 23.6；真实 FDT 已生成最小 ADT；本地链接指针已逐条审计，见 docs/xnu-preparation.md；组件 prelink 见下方单独一行 |
| 源码 XNU 装载与 boot_args 入口检查点 | 真机验收通过 | EL1 读取参数后 HVC 返回 EL2，Linux 收到唯一且通过校验的 copied／checkpoint／boot_args／current EL 四字段，见 docs/xnu-checkpoint.md |
| XNU MMU 开启检查点 | 真机验收通过 | 完整 8192 字节日志确认 MMU／表池／硬件 AT 校验均成功，并返回 Linux；见 docs/xnu-mmu-checkpoint.md |
| XNU 高虚拟地址／C 入口栈读取检查点 | 真机验收通过 | 原始 RET 与栈帧调整执行，高地址／栈读取／C 入口三个字段为 1；完整 8192 字节日志通过校验，见 docs/xnu-highva-checkpoint.md |
| XNU 早期 C 正文／参数复制检查点 | 真机验收通过 | 完整日志的 C 正文／栈写入／参数复制／缓存维护四字段及原有 11 字段通过，完整父子帧与参数内容核对后返回 Linux；见 docs/xnu-cinit-checkpoint.md |
| XNU 启动 CPU 数据初始化检查点 | 真机验收通过 | 完整 16 字段通过；原始标量／FP/NEON 写入后的 CPU 对象、保留哨兵和 guard 全部核对，并返回 Linux；见 docs/xnu-cpu-checkpoint.md |
| XNU 平台入口与首次原生 ADT 查找 | 真机验收通过 | 完整 19 字段通过；正常 CPU 初值、PE／DT 全局、原生 arm-io 查找、独立节点／游标及完整活动栈帧核对后返回 Linux，见 docs/xnu-platform-checkpoint.md |
| XNU 完整早期平台返回 | version7 真机 22 字段通过（2026-10-30） | 完整 PE_init_platform(FALSE) 返回、受限输入的型号／时钟结构／配置、父帧／表池／AT／缓存与正常 Linux 交接通过；频率缺失仍保留，见 docs/xnu-pe-checkpoint.md |
| 当前包装器的 mode6 回归对照 | 真机 19 字段通过（2026-10-30） | 原样已验收负载在当前公共装载／返回路径仍可工作；不验证后续执行或 mode7／8 专属门限，见 docs/hardware-tests.md |
| XNU SoC 属性诊断中点 | version9 真机 20 字段通过（2026-10-30） | 原生属性读取／缓存对象、CPU／PE／DT、活动帧、表池／AT／缓存返回通过；未访问 SoC MMIO 或验证完整平台返回，见 docs/xnu-soc-checkpoint.md |
| 同步异常／返回失败诊断 | version10 主动异常传输已真机验收（2026-10-30） | 8192 字节日志通过 46 项必需字段读取，精确 BRK／向量／页表池／缓存传输门匹配并返回 Linux；version10 的 checkpoint=0 符合故障设计，后续 version9 正常 SoC 返回已验收，见 docs/xnu-diagnostics.md |
| XNU WFE／单 CPU 拓扑 | version11 真机 40 字段通过（2026-10-31） | 原始 WFE 配置、完整单 CPU 拓扑与父帧／PE／表池／AT／缓存门通过；三个系统寄存器按入口值恢复，随后交接 Linux，见 [契约](xnu-topology-checkpoint.md) |
| XNU 内核初始化 | 屏幕输出镜像已到达等待根设备 | 屏幕照片显示 VM、IOKit、BSD、网络栈初始化完成并打印 `Waiting on ... IOMedia`；无逐字段核对。定时器中断已确认，SMP 未确认，有一个未查明的随机 zone panic，见 [硬件记录](hardware-tests.md) |
| 第一个用户态程序 | 屏幕输出镜像已从内存盘运行 PID 1（2026-10-31） | 屏幕照片显示 `/sbin/launchd` 加载和 `init running as PID 1`，即 EL0 执行及 `open`、`getpid`、`write` 系统调用可用；每 5 秒的 `select` 定时唤醒也已工作，说明定时器中断正常。程序是自写的静态测试程序，不是 launchd，见 [硬件记录](hardware-tests.md) |
| 组件 prelink | 真机通过，运行自写 init（2026-10-31） | 屏幕照片显示 OS profile 的 Apple 原始组件参与启动（共 31 个，其中 `IOHIDEventDriver` 无代码），APFS、AppleImage4 等有加载输出，真实 AMFI 通过自建 trust cache 放行 init，tick 超过了之前 `AMFI: No PMGR?` 的 panic 位置；ADT 没有这些组件对应的硬件节点，多数驱动没有实际工作，见 [硬件记录](hardware-tests.md) |
| Apple launchd 与恢复环境 | 真机运行（2026-10-31） | 屏幕照片显示 IPSW 的 Update 恢复内存盘以 APFS 根运行，原版 launchd、dyld、`restored_update` 在原版 trust cache 下执行，`restored_update` 因没有 IOWatchdog 服务按自身逻辑退出，随后等待 IONVRAM 和 NAND 服务，内核保持存活。这不是主系统，见 [硬件记录](hardware-tests.md) |
| USB 主机与大容量存储 | 探针只读实验真机通过；XNU 内核只读读取屏幕确认（2026-10-31） | 探针用 libpayload 枚举到硬盘并读到 `EFI PART`、`CHROMEOS`，严格字段通过，见 [USB 只读实验](usb-read.md)。XNU 内核里的同一套栈也读到这三个扇区，只有屏幕证据；IOKit 块设备和 GPT 分区未确认，见 [硬件记录](hardware-tests.md)。既定路线：根文件系统先从外接盘启动，之后才考虑 eMMC |
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
5. 已确认 current EL = 1 且 stage = 3，handoff EL = 2；首轮正常路径未触发异常；后续 version10 已取得精确 BRK 的 ESR／ELR／FAR／SPSR 现场并通过异常回收验收，其他故障类别仍需分别验证。
6. 后续已完成：源码 XNU 的运行时段装载与 pre-MMU boot_args 读取检查点，取得有效硬件日志。随后 MMU 开启、完整页表池和硬件 AT 检查点也已通过真机验收。随后高虚拟地址取指、C 入口栈帧调整与诊断栈读取也已取得硬件日志。早期 C 正文的原始栈保存、参数复制和缓存清理返回也已取得硬件日志。启动 CPU 对象的原始标量／SIMD 初始化也已通过硬件日志验收。原始平台状态、ADT 绑定及首次原生 arm-io 查找已通过完整 19 字段和独立清单核对。后续 version9 的 SoC 属性读取／缓存写入也已取得严格 20 字段硬件记录。随后 version7 的完整 PE 返回也已取得严格 22 字段硬件记录。version11 的 WFE 配置／单 CPU 拓扑解析返回也已取得完整 40 字段硬件记录。待完成：线程／VM、平台／中断／SMP 初始化、其余 ADT 消费、平台接口与显示恢复。

原生探针、源码 XNU pre-MMU 入口、MMU 开启、高地址入口栈读取、早期 C 参数复制、启动 CPU 数据及首次平台／ADT 查找检查点均已取得硬件日志；主机测试仍须单独列出。没有完整 XNU 或 iPadOS 启动结果。

2026-10-31：version11 已补齐上述 CNTKCTL 生命周期，源与主机系统验证及原生产物审查通过。最终镜像通过目标端签名／传输核验后，已写入既有可清空外置介质并完成全量 128 MiB 回读；没有自动重启；当时尚待手动启动，随后真机验收结果见下文。

2026-10-31：version11 已取得完整 8192 字节硬件日志，严格 40 字段、精确拓扑返回 PC／参数布局、全部继承状态与缓存覆盖通过，正常 boundary=1／diagnostic return=0，恢复三个系统寄存器后进入原生 Linux。本次入口值均为零，非零初值恢复的证据仍是三组主机模型；WFE 参数配置不等于计时器中断或睡眠唤醒支持。显示和线程／VM 仍未实现／验收。


2026-10-31：独立显示发现真机完整 42 字段通过，13 次固定读取无异常、零硬件写入后返回 Linux；TE／DSI 均为零。几何与格式匹配，但 scanout 未满足当前 1 MiB 对齐条件，并命中复用 XNU 放置范围门的整体固件区排除，候选为假；该门短路后其余 FDT 排除尚不能判定。先核实实际分配与保留归属，再限定独立显示实验；未绘图、未执行 EC 事务，未更改 XNU 验收边界。


2026-10-31：scanout 分配与归属只读核实完成。分配器对齐的是末端而不是基址，按此重放私有内存表与真机读数逐位相等；整体排除带是 XNU arena 放置策略，不再用于判断现存显示对象。当次设备树没有其他已识别拥有者，Linux 把这段范围当普通 RAM。当次 memreserve、已安装固件二进制、SMMU／MDP／缓存及其他 DMA 主设备仍未证实，见 [核实记录](display-discovery.md#2026-10-31真实分配与保留归属核实)。schema 2 分开输出元数据、分配关系与显示归属，已通过真机 45 字段，当次范围门在原始 FDT 上运行；图案、背光和可见画面仍未实现或验收。
