# Coachz 硬件尝试记录

记录更新：2026-10-27。包括早期外置启动行为观察，以及后续 Linux 接收的直接探针日志；不同证据层级分开记录。

## 已观察到的行为

| 源码提交 | 模式 | 预期行为 | 观察 | 当轮判定 |
| --- | --- | --- | --- | --- |
| f3ec9a2 | reset-el2 | 诊断通过后等 3 秒复位 | 黑屏，未观察到预期复位 | 未通过；停止位置未知 |
| 4054f94 | reset-el2 | 诊断通过后等 3 秒复位 | 报告 10 秒以内自动重启，没有精确计时 | 时间范围与预期兼容，但不能单独确认检查点 |
| 4054f94 | reset-el1 | 诊断通过后等 10 秒复位 | 报告与上一轮时间相近，没有精确计时 | 未形成预期的成对延时差异，EL1 未确认 |
| 4054f94 | memory | 记录后停住，不主动复位 | 持续黑屏，没有自动重启；未提供精确观察时长 | 与不复位行为相符；不能据此确认 EL1 或 stage=3 |
| 494ce12 工具派生自 4054f94 | reset-el1-no-reset | 仅将最终复位 SMC 换为 NOP，其余 Image 字节相同 | 不自动重启 | 与原版形成匹配对照，支持最终复位调用及此前 EL1／诊断门限的间接执行证据 |

两个修复版镜像在写入测试介质后均对全部 64 MiB 回读计算 SHA-256；容器签名及 body 也经目标系统的 futility 验证。

| 镜像 | SHA-256 |
| --- | --- |
| reset-el2.img | 206b63b26aeb58f2ef2c0a2251a8153767961f6fb04921b66d099d533e81f861 |
| reset-el1.img | ce8776772908b42e716a5f58662438569dbd0e4410ce9e5335f1f498aa584b3f |

反汇编确认两个检查点的计数器等待逻辑包含 3 / 10 秒分支，EL1 版本传入目标 EL=1。主机构建与签名检查不能证明这些指令已在目标 CPU 执行。

EL1 尝试之后读取的 EC GET_UPTIME_INFO 返回 AP reset count=0，recent_ap_reset 为空。[Linux 协议定义](https://github.com/torvalds/linux/blob/v6.12/include/linux/platform_data/cros_ec_commands.h#L5164-L5208)说明该接口统计 EC 发起的 AP 复位；空记录不能排除 AP 自身、看门狗或其他固件复位。可读的固件日志只覆盖随后一次正常内置启动，没有上一轮探针的 ESR / ELR / FAR。

仅凭普通 memory 对照时，差异仍同时涉及 PSCI_VERSION、计数器等待、SYSTEM_RESET 和编译布局。后续单指令匹配对照保持不重启，而原版会自动重启，因而对最终 SYSTEM_RESET 是复位来源提供了较强的间接证据。按原版的控制流，该调用之前必须满足 current EL=1、stage=3、FDT 合法、PSCI 版本与计时频率检查。因此该轮可以记录 EL1 诊断路径已有间接硬件执行证据，但不能冒充已取回寄存器读数；该轮没有直接观测入口 EL、ERET 本身、精确延时或日志内容。显示恢复、XNU 与 iPadOS 尚未实现。

## 已执行：不主动复位的对照

直接使用提交 4054f94 已构建的 `memory` 版本，不修改引导入口或外设设置：沿用 EL2→EL1 与 FDT 诊断，返回后进入 WFE；不从 probe_main 调用 checkpoint_run，也不发送 PSCI 请求。

已准备的普通文件镜像：

| 镜像 | SHA-256 |
| --- | --- |
| memory.img | 35d9c9000c843cec0cc09d0b252a0c8ad636d0c9c7446f9261a0103a7735109d |
| memory.kpart | 29bf3760ca76362ab75e63fc118e64c13fd5b2dc201e2219d26bd89147c415ef |

该产物的 ELF / Image / FIT 与原构建清单哈希一致；ELF 标量检查、FIT 内嵌字节检查、签名和 body 校验通过；写入介质后的全部 64 MiB 回读哈希一致。硬件观察为持续黑屏、不自动重启，与定时版本不同。

- 若同样在相近时间自动重启，说明这种观察本身不能归因于定时 SYSTEM_RESET，需调查其他复位来源和外置启动路径。
- 若 30 秒内不重启，说明行为与定时版本不同，但仍不能据黑屏保持判断 EL1 或 stage=3 成功；异常或 FDT 检查失败同样可能停住。
- 黑屏保持后可手动恢复正常内置启动；记录手动复位，不能将其计入自动重启时间。

## 已执行：单指令匹配对照

[匹配对照](control.md)的 Image 仅在 0x272c 将 SMC #0 的四个字节换为 NOP；其余 Image 字节及 FIT 的非内核数据／哈希属性相同。镜像 SHA-256 为 `03d8e18a0788ff9581d8da33f8e5748ba03d18c92655a30bfe0f7a2e6559fa0c`，签名／body 与全部 64 MiB 介质回读校验通过。硬件反馈为不重启。

这比普通 memory 对照更直接地隔离了最终复位调用。该对照的当轮里程碑是间接执行证据；后续直接日志见下一节。两者均不代表已有 XNU 或 iPadOS 运行结果。

## 2026-10-27：原生 Linux 接收成功

源码提交 `bb29e0e`；测试镜像 `linux-log.img` 的 SHA-256 为 `a533cc917bdd55a54c24b6f67e0d83e671890330e1dee989dea17af2c48be8a4`。从外置镜像经原厂开发者模式启动后，原生 Linux 6.12.42-stb-cbq+ 可通过 SSH 访问。

在运行中的 Linux 读取 `/sys/firmware/devicetree/base/chosen/trogdor-ipados,probe-log`，完整 8192 字节属性通过读取器校验：日志头、NUL 终止与尾部零填充有效，四个必需字段唯一且满足：

```text
entry EL=0x0000000000000002
current EL=0x0000000000000001
stage=0x0000000000000003
handoff EL=0x0000000000000002
```

日志包含 phase-0 完成标记、入口 SCTLR／HCR、MIDR／MPIDR／CNTFRQ 以及 FDT 内存、initrd 和 Coreboot 表属性。完整原始日志仅私下保存；本地副本与运行中 Linux 属性的 SHA-256 一致，构建清单中的源文件哈希与提交源码一致。

这是原厂 EL2 入口→探针 EL1 诊断→受控 HVC 返回 EL2→原生 Linux 接收日志的直接硬件证据，首阶段探针验收通过。没有观测到异常路径，也没有加载 XNU 或 iPadOS。Linux 自身能恢复显示不等于独立探针已实现显示驱动。

## 后续待验证

原生探针日志已取回。源码 XNU pre-MMU 入口检查点已通过真机日志验收；探针显示恢复和异常日志接收仍需独立验证。


## 2026-10-27：源码 XNU 入口检查点介质准备

本节仅记录介质准备；随后的真机结果见下一节。使用真实源码构建的 XNU 10063.141.1，插入四字节 pre-MMU HVC 检查点，装载与返回门见 [入口检查点](xnu-checkpoint.md)。51 项主机回归与独立源码审查通过；原有四种探针仍可构建。

| 产物 | SHA-256 |
| --- | --- |
| 源码内核 | 6b406b8cd4ee5a6c11793256d8cadc5453365e8e8edb5f487953e02e259c7e9b |
| instrumented kernel | 49426594554a6e07164599c9514d69da340dde7e9dbdfa8b94075835fe718943 |
| linux-log.kpart | 3357afbe3d1a81ffeaadf669986eb27f09d804e9acd0e83c1efdd2e93ba67abb |
| linux-log.img | 5a385131f66934983e96e913b4f6e4bc9a39a79256a9da740238fcfafa5910ec |

目标 Debian 独立完成 vboot 签名、body、GPT 与传输哈希校验。写入已核对的可清空外置介质后，完整 128 MiB 回读 SHA-256 一致。写入器验证 USB／removable、容量、根分区、boot、swap 与独占打开；未写固件或内置盘，没有自动重启。

待从原厂开发者模式启动，再用 `read_probe.py --xnu-checkpoint` 验证同次引导日志。仅返回 Linux、保持黑屏或自动重启均不能单独证明 XNU 读取了 boot_args。


## 2026-10-27：源码 XNU pre-MMU 入口检查点成功

执行的源码版本为 `d323df8`；镜像 SHA-256 为 `5a385131f66934983e96e913b4f6e4bc9a39a79256a9da740238fcfafa5910ec`。从外置介质经原厂开发者模式启动后，原生 Linux 6.12.42-stb-cbq+ 可访问，同次启动的完整 8192 字节日志通过 `read_probe.py --xnu-checkpoint` 校验：

```text
entry EL=0x0000000000000002
current EL=0x0000000000000001
XNU copied=0x0000000000000001
XNU checkpoint=0x0000000000000001
XNU boot_args verified=0x0000000000000001
XNU current EL=0x0000000000000001
stage=0x0000000000000003
handoff EL=0x0000000000000002
```

以上字段均唯一，日志头、NUL 终止和尾部零填充有效。本地私有原始记录与目标属性的独立 SHA-256 一致；返回 PC／boot_args 地址另与构建负载中的检查点偏移、段跨度进行主机换算，结果一致。原始设备树、地址及完整日志没有纳入仓库。

这证明实际源码 XNU 的冷启动前缀已在 EL1 执行：段装载完成，内核读取的 boot_args 指针和 virtBase／physBase／memSize／bootFlags，以及页表地址和 M/C/I 关闭状态通过返回门校验，随后 EL2 状态恢复并交接原生 Linux。它不证明 XNU 已消费 ADT、写入页表、开启 MMU、进入 C 初始化或运行 iPadOS 用户态。


## 2026-10-27：MMU 开启检查点介质准备

本节记录启动前的主机验证与介质准备；随后的真机结果见下一节。新模式保留 16 KiB 块内偏移，在真实源码内核的 MMU 开启和 ISB 后插入 HVC；返回门同时校验完整页表池、寄存器与硬件 AT 结果，见 [MMU 检查点](xnu-mmu-checkpoint.md)。源码内核保持原始版本与全部本地链接指针，新旧模式各只替换各自的一条指令。

| 产物 | SHA-256 |
| --- | --- |
| instrumented MMU kernel | e30a7616659b43d48ef3caeca258da8a1a5f8183e77937243785923f966ee039 |
| linux-log.kpart | 995c08742c954bee52e810b7a89ecea3c66751d9d77747b35c855e6505bed62d |
| linux-log.img | d6e1b88dc2ce61bfa47e476ea835fc3d4f1966be7085cda5857a63e5b68b692e |

59 项主机回归、四种原有探针构建和独立源码审查通过。实际包装 Image 仍申请 74 MiB。目标 Debian 独立通过传输哈希、vboot 签名／body 与 GPT 校验；已核对的可清空外置介质写入后，完整 128 MiB 回读哈希一致。写入器再次核对 USB／removable、容量、根分区、boot、swap 与独占打开；未写固件或内置盘，没有自动重启。

待手动从原厂开发者模式启动，用 `read_probe.py --xnu-mmu` 取得新的同次引导日志。旧入口检查点的成功不能外推为本轮 MMU 成功。


## 2026-10-27：MMU 开启检查点成功

源码版本 `62cf0e6`，镜像 SHA-256 为 `d6e1b88dc2ce61bfa47e476ea835fc3d4f1966be7085cda5857a63e5b68b692e`。经原厂开发者模式启动后，原生 Linux 6.12.42-stb-cbq+ 可访问。完整 8192 字节同次引导日志通过 `read_probe.py --xnu-mmu`；八个原有路径／XNU 字段与三个新增 MMU 字段均唯一且满足预期：

```text
entry EL=0x0000000000000002
current EL=0x0000000000000001
XNU copied=0x0000000000000001
XNU MMU enabled=0x0000000000000001
XNU tables verified=0x0000000000000001
XNU translations verified=0x0000000000000001
XNU checkpoint=0x0000000000000001
XNU boot_args verified=0x0000000000000001
XNU current EL=0x0000000000000001
stage=0x0000000000000003
handoff EL=0x0000000000000002
```

日志头、NUL 终止、尾部零填充有效。主机私有副本与设备属性独立 SHA-256 一致；源码清单与当前执行源码匹配。按实际 payload version=2、kernel VA 的块内偏移、完整段跨度和检查点 VA 重新换算，返回 PC 与 boot_args 地址一致。没有公开原始设备树、地址或完整日志。

这确认源码 XNU 在 EL1 构造页表并开启 M/C/I：完整 64 KiB 表池和寄存器通过返回门校验，EL2 的 AT S1E1R 对两个高虚拟地址返回预期物理页，之后恢复 EL1／EL2 状态并返回原生 Linux。本轮在 identity 映射上执行到 HVC，尚未从高虚拟地址取指或进行栈访存，不证明已进入 C、消费 ADT、启动驱动或运行 iPadOS。


## 2026-10-27：高虚拟地址与 C 入口栈读取介质准备

本节记录启动前介质准备；随后的真机结果见下一节。保留真实源码 cold start 到 RET 的路径，以及 arm_init 入口的第一条 SUB SP；把下两条 STP 换为 LDR x19,[sp,#0x70] 和 HVC。它验证高地址取指、参数和栈读取的条件，不执行 C 初始化正文；详见 [高VA检查点](xnu-highva-checkpoint.md)。

| 产物 | SHA-256 |
| --- | --- |
| instrumented high-VA kernel | 2b31b91b9a6179df3176033775ba1797d665446df767da0394b60bd6f0314b75 |
| linux-log.kpart | d5ef4abff1837a36e171973bd4c8cb463cd67c47f19ac9f65f446f5991a8cc20 |
| linux-log.img | 3c61dd4213cdc0918d280896b5eb81d49f2020062467b4a3a052b1d600421d7f |

66 项主机回归、四种原有探针构建和独立审查通过。最终实际生成码反汇编确认 SUB/LDR/HVC 编码，包装 Image 仍为 74 MiB 内存申请。目标 Debian 独立通过传输哈希、vboot 签名／body 和 GPT 校验；已核对的可清空外置介质写入后，完整 128 MiB 回读哈希一致。写入器核对 USB／removable、容量、root／boot／swap 与独占打开；没有写固件或内置盘，没有自动重启。

待手动通过原厂开发者模式启动，再用 `read_probe.py --xnu-highva` 验证原有全部路径／XNU／MMU 字段和新的高地址／栈读取／C 入口字段。前一轮 MMU 成功不能外推为本轮高地址执行成功。


## 2026-10-27：高虚拟地址与 C 入口栈读取成功

源码版本 `4cfcb29`；镜像 SHA-256 为 `3c61dd4213cdc0918d280896b5eb81d49f2020062467b4a3a052b1d600421d7f`。经原厂开发者模式启动后回到原生 Linux 6.12.42-stb-cbq+，完整 8192 字节同次引导日志通过 `read_probe.py --xnu-highva`：

```text
entry EL=0x0000000000000002
current EL=0x0000000000000001
XNU copied=0x0000000000000001
XNU MMU enabled=0x0000000000000001
XNU tables verified=0x0000000000000001
XNU translations verified=0x0000000000000001
XNU high VA verified=0x0000000000000001
XNU stack read verified=0x0000000000000001
XNU C entry verified=0x0000000000000001
XNU checkpoint=0x0000000000000001
XNU boot_args verified=0x0000000000000001
XNU current EL=0x0000000000000001
stage=0x0000000000000003
handoff EL=0x0000000000000002
```

14 个验收字段均唯一且满足预期，日志头、NUL 终止与零填充有效。本地私有原始记录与设备属性的独立 SHA-256 一致。精确高地址返回 PC、boot_args VA／PA 关系与实际 version=3 负载的检查点、偏移和参数位置换算一致，源码清单匹配。没有公开原始地址、设备树或完整日志。

硬件返回门同时核对栈帧、诊断 LDR 的零值、x0 KVA、LR、CPACR／TPIDR、完整 64 KiB 表池，以及参数／代码／栈槽的硬件 AT。由此确认原始 RET 到达 arm_init 高虚拟地址，执行原始 SUB 与诊断栈读取，随后完成缓存行维护、状态恢复并交接 Linux。没有执行原始 STP 或 C 初始化正文，没有验证栈写入、ADT 消费、驱动或 iPadOS 用户态。


## 早期 C 正文与参数复制检查点：介质准备

本节仅记录启动前准备。新模式保留 arm_init 原始栈保存、最早 C 正文、完整参数复制和 BootArgs 设置，在 cpu_data_init 前 HVC 返回；详见 [C 初始化检查点](xnu-cinit-checkpoint.md)。74 项主机回归、四种探针构建与独立审查通过。实际生成码确认完整 arena 的 CIVAC／DSB 在 EL1 M/C/I 关闭之前；完整父／子帧与参数复制有逐字节破坏拒绝测试。

镜像 SHA-256 `5ce7714eacccd5e527b9b1d6e0d4ee05626e83ce1f0c069f617c9de9227ab119`，签名容器 SHA-256 `ee29ea075c3d6bbeed686c834c8e70058042a7c4309356380cfcb8a7367fbd23`。签名／body、主备 GPT CRC 和内嵌容器通过；目标端传输哈希一致，固定哈希写入器完成外置介质全部 128 MiB 回读。写入前再次核对 USB／removable、容量、root／boot／swap，独占打开目标，没有写固件或内置盘，没有自动重启。

本节为启动前准备记录；随后真机结果见下节。`read_probe.py --xnu-cinit` 要求原有 11 字段和 C 正文／栈写入／参数复制／缓存维护四个新字段，并独立检查精确返回 PC 与负载。上一轮高 VA 栈读取成功不能代替本轮 C 写入或缓存返回验收。


## 2026-10-27：早期 C 正文、完整栈写入与参数复制成功

执行源码版本 `92fe75d`，镜像 SHA-256 `5ce7714eacccd5e527b9b1d6e0d4ee05626e83ce1f0c069f617c9de9227ab119`。经原厂开发者模式启动后，原生 Linux 6.12.42-stb-cbq+ 可访问，完整 8192 字节日志通过 `read_probe.py --xnu-cinit`：

```text
entry EL=0x0000000000000002
current EL=0x0000000000000001
stage=0x0000000000000003
handoff EL=0x0000000000000002
XNU copied=0x0000000000000001
XNU checkpoint=0x0000000000000001
XNU boot_args verified=0x0000000000000001
XNU current EL=0x0000000000000001
XNU MMU enabled=0x0000000000000001
XNU tables verified=0x0000000000000001
XNU translations verified=0x0000000000000001
XNU C body verified=0x0000000000000001
XNU stack writes verified=0x0000000000000001
XNU args copy verified=0x0000000000000001
XNU cache maintenance verified=0x0000000000000001
```

15 个字段均唯一且满足预期，日志头／终止／填充有效；设备属性与本地私有原始记录的独立 SHA-256 一致。独立核对 version=4、精确 C 复制后返回 PC、参数 VA／PA 与对齐偏移、源内核／补丁内核／负载／镜像及实际代码清单均匹配。64 字节缓存行 × 393216 行恰好覆盖 24 MiB；新记录与前一轮不同。

硬件返回门验证了完整 0x480 字节复制、BootArgs 指针、slide 路径全局、guard 与完整 0xe0 字节父／子栈帧，以及原有现场、完整表池和参数／代码／栈硬件 AT。由此确认原始 C 序言和最早 C 正文确实执行，原始 memmove 完成参数复制，全局指针已设置，并经过缓存清理、状态恢复返回 Linux。没有公开原始地址、设备树或完整日志。

检查点仍在 cpu_data_init 前，未进入后续 CPU／平台初始化，也未消费 ADT 或启动 iPadOS。


## 2026-10-27：启动 CPU 数据检查点介质准备

本节仅记录启动前准备。version=5 保留原始 cpu_data_init 调用，在其返回后、PE_init_platform 前 HVC 停止；FP/NEON 的使能与完整 CPU 对象哨兵检查见 [CPU 数据检查点](xnu-cpu-checkpoint.md)。82 项主机回归、四种原有探针构建和独立审查通过，实际生成码的补丁范围、CPACR／ISB 与标量引导器检查通过。

镜像 SHA-256 `baae4bfebea2d3a7174be3f63785c058c0f47d9a3b3b89cfdfe90a7d68b1a1c3`，签名容器 SHA-256 `898eb0b2065b8e9c2b122a334bb497576ee460822c69f1356a323f57d54aaf49`。主机与目标端签名／body、传输哈希、实际 GPT 主备 CRC 和内嵌容器通过。重新核对外置介质的 USB／removable、容量、root／boot／swap 后，固定哈希写入器独占打开并完成全部 128 MiB 写入与回读；没有写入固件或内置盘，没有自动重启。

本节为启动前准备记录；随后真机结果见下节。`read_probe.py --xnu-cpu` 要求既有 C 初始化 15 字段和新的 CPU 数据字段，并独立核对本轮精确返回 PC、version=5 和负载清单。前一轮 C 参数复制验收不能代替本轮原生 SIMD／CPU 初始化验收；诊断哨兵不能用于继续平台启动。


## 2026-10-27：原始启动 CPU 数据与 SIMD 写入成功

执行源码版本 `4541792`，镜像 SHA-256 `baae4bfebea2d3a7174be3f63785c058c0f47d9a3b3b89cfdfe90a7d68b1a1c3`。经原厂开发者模式启动后，原生 Linux 6.12.42-stb-cbq+ 可访问，完整 8192 字节日志通过 `read_probe.py --xnu-cpu`：

```text
entry EL=0x0000000000000002
current EL=0x0000000000000001
stage=0x0000000000000003
handoff EL=0x0000000000000002
XNU copied=0x0000000000000001
XNU checkpoint=0x0000000000000001
XNU boot_args verified=0x0000000000000001
XNU current EL=0x0000000000000001
XNU MMU enabled=0x0000000000000001
XNU tables verified=0x0000000000000001
XNU translations verified=0x0000000000000001
XNU C body verified=0x0000000000000001
XNU stack writes verified=0x0000000000000001
XNU args copy verified=0x0000000000000001
XNU cache maintenance verified=0x0000000000000001
XNU CPU data verified=0x0000000000000001
```

16 字段唯一且满足预期，日志头、终止与零填充有效；设备属性和私有原始副本的独立 SHA-256 一致。精确 CPU 调用返回 PC、version=5、参数地址与对齐偏移、CPU／时钟符号，以及源内核／补丁内核／负载／镜像／实际代码清单独立核对通过。新记录与前一轮不同，64 字节缓存行 × 393216 行覆盖全部 24 MiB 自有区间。没有公开原始地址、设备树或完整日志。

返回门验证完整 0x500 字节 CPU 对象与两侧共 32 字节 guard；应清零字段、五处常量／指针、未写字段的 0xa5 和 guard 零值全部匹配。现场确认 FPEN、A=0、实际 CPU 参数与 LR，原有完整参数复制、父／子栈帧、完整表池和硬件 AT 也通过，随后恢复状态并返回 Linux。

由此确认原始 cpu_data_init 叶函数、标量与 FP/NEON 写入在有限诊断输入下执行成功。仍停在 PE_init_platform 前；诊断哨兵不能用于继续平台启动，ADT、中断、SMP、平台驱动与完整 iPadOS 均未验证。


## 2026-10-27：平台入口与原生 ADT 查找介质准备

本节仅记录启动前准备。version=6 使用正常零初值 CPU 对象，保留 PE_init_platform(FALSE)、SecureDTInit 和原始机器识别调用链中的首次 arm-io 查找，在返回后、SoC 属性读取前 HVC；详见 [平台／ADT 检查点](xnu-platform-checkpoint.md)。91 项主机回归、四种探针构建与独立审查通过，实际生成码的单字补丁、FP 使能和标量包装器约束检查通过。

镜像 SHA-256 `5fef36cd41fc2f53890f89266bbb4f557f8ded4b0f12dd44b1bd2965d5b1f399`，签名容器 SHA-256 `f87c09aa0b8c827107c902c9e7b579812868ea93b5333d84c2a822caf367c95e`。主机与目标端签名／body、传输哈希、实际主备 GPT CRC、内嵌容器和代码清单通过。重新核对外置介质 USB／removable、容量、root／boot／swap 后，固定哈希写入器独占打开并完成全部 128 MiB 写入和回读；没有写入固件或内置盘，没有自动重启。

本节为启动前准备记录；随后真机结果见下节。`read_probe.py --xnu-platform` 要求既有 16 字段和平台状态／ADT 绑定／ADT 查找三个新字段，并独立核对本轮精确返回 PC、version=6 和负载清单。此前 CPU 初始化通过不能代替本轮原生 ADT 遍历验收。


## 2026-10-27：原始平台状态、ADT 绑定与原生查找成功

执行源码版本 `f30464b`，镜像 SHA-256 `5fef36cd41fc2f53890f89266bbb4f557f8ded4b0f12dd44b1bd2965d5b1f399`。经原厂开发者模式启动并交接原生 Linux 6.12.42-stb-cbq+，完整 8192 字节日志通过 `read_probe.py --xnu-platform`：

```text
entry EL=0x0000000000000002
current EL=0x0000000000000001
stage=0x0000000000000003
handoff EL=0x0000000000000002
XNU copied=0x0000000000000001
XNU checkpoint=0x0000000000000001
XNU boot_args verified=0x0000000000000001
XNU current EL=0x0000000000000001
XNU MMU enabled=0x0000000000000001
XNU tables verified=0x0000000000000001
XNU translations verified=0x0000000000000001
XNU C body verified=0x0000000000000001
XNU stack writes verified=0x0000000000000001
XNU args copy verified=0x0000000000000001
XNU cache maintenance verified=0x0000000000000001
XNU CPU data verified=0x0000000000000001
XNU platform state verified=0x0000000000000001
XNU ADT binding verified=0x0000000000000001
XNU ADT lookup verified=0x0000000000000001
```

19 字段唯一且满足预期，日志头、终止与零填充有效；设备属性和私有副本的独立 SHA-256 一致。精确原生查找返回 PC、version=6、参数地址与对齐关系、CPU／时钟／平台符号，以及源内核／补丁内核／负载／镜像／实际代码清单独立核对通过；平台元数据的节点与游标偏移和主机独立 ADT 解析一致。新记录与前一轮 CPU 检查点不同，64 字节缓存行 × 393216 行覆盖全部 24 MiB 自有区间。

返回门确认正常零 CPU 初值下的完整 CPU 对象、PE_state、page_protection_type、DT 根／大小／末尾／初始化标志、查找游标与输出节点均符合期望；四层活动栈帧、4 KiB 栈范围底部 guard、原有参数复制、完整表池和硬件 AT 通过。随后完成状态恢复并返回 Linux。原始地址、设备树、完整日志及日志哈希未公开。

这确认原始平台状态初始化、SecureDTInit 和首次原生 SecureDTFindEntry 遍历成功，不再只是主机解析树。检查点停在查找返回后、后续 SoC 属性读取前；MMIO、中断、SMP、平台驱动与完整 XNU／iPadOS 启动尚未验证。


## 2026-10-27：完整早期平台返回介质准备

本节记录启动前准备，不是 version=7 的真机执行结果。沿现有非零 SoC ranges 和唯一 running CPU，保留完整 PE_init_platform(FALSE) 路径，在 arm_init 的调用返回处单条 HVC；合并 SoC 属性、CPU／时钟信息、根型号与早期调试配置。缺失的五项频率属性保持零字段，不能视为时钟支持；详见 [完整 PE 返回检查点](xnu-pe-checkpoint.md)。

98 项主机回归、四种既有探针构建与独立源码／实现审查通过。任意层级重复兄弟节点的 Python／C 拒绝规则已统一并回归。最终包装器 6366 条指令、13 段满足标量约束，实际补丁只替换平台调用返回后的 timer 配置 BL；完整平台函数及此前源码保留。

镜像 SHA-256 `b15cc8ac07b6823835a7edc731b4daa18323c21c3d9da13f19267cd35549733e`，签名容器 SHA-256 `ca3685e83a043df599c1dee4e73de54ce5942ea4f8c9a695b663049cbeb2da9d`。实际主备 GPT CRC／分区属性／内嵌容器、最终源码清单和全部产物哈希匹配。主机及目标端 vboot 签名与 body 校验通过，传输哈希一致，保留 Linux、47 份完整 DTB、initrd 和命令行。

重新核对 USB／removable、容量、root／boot／swap 后，固定哈希写入器独占打开外置介质并完成全部 128 MiB 写入和回读，哈希一致。没有写固件或内置盘，没有自动重启；私有写入器与原始设备数据不进入公开仓库。

待手动启动后，以 `read_probe.py --xnu-pe` 要求全部 22 字段，并独立核对精确 arm_init 返回 PC、version=7、参数地址和负载清单。此前 version=6 的 19 字段不能代替本轮完整平台返回验收。


## 2026-10-27：完整平台返回首轮黑屏与诊断对照

执行源码版本 `8c73efb`，镜像 SHA-256 `b15cc8ac07b6823835a7edc731b4daa18323c21c3d9da13f19267cd35549733e`。手动启动后持续黑屏，未取得精确时长；随后恢复内置 Linux，设备树没有本轮 probe-log。不能据此确认已到平台返回门，也不能区分 XNU 内部停止与返回校验失败；本轮未通过验收。

主机侧重放实际源码的 CPU 初始化和完整 PE 指令可到达精确 HVC，SoC、PE／DT、型号、时钟／配置及活动父帧与模型一致。这是主机对照，不能替代硬件执行证据。

下一份诊断对照保持同一 XNU payload／ADT／单字 HVC 补丁，保留完整捕获现场、硬件 AT、缓存计数和全部页表池硬门限，仅允许后续普通数据校验失败时带失败字段返回 Linux。成功字段不会被改为通过；严格读取拒绝 diagnostic return=1。未到 HVC 或未满足捕获／页表门限仍停机，根因尚未定位。99 项主机回归与独立审查通过，详见 [诊断返回契约](xnu-pe-checkpoint.md)。


## 2026-10-29：诊断返回对照介质准备完成

执行源码版本 `d7c7837`，诊断镜像 SHA-256 `81468498db2ac78c6f9ffbb5bf07b2404c65422bb4d2d3c544cc7f2d37bf67dc`，签名容器 SHA-256 `e6a2ab5e45d0120334ca102e4c11d9ad1d7069a2ff66763fb0b41e3c3d03f62c`。最终源码／产物清单重新核对一致；目标端镜像、容器与日志读取器的传输哈希匹配，vboot keyblock／签名／body 校验通过。

重新核对外置介质 USB／removable、容量和未挂载状态，排除 root／boot／swap 后，固定哈希私有写入器独占打开介质，完成全部 128 MiB 写入、同步与完整回读。回读 SHA-256 与本诊断镜像一致，不能用此前首份镜像的回读结果替代。没有写入固件或内置盘，没有自动重启；私有写入器和原始设备数据不进入公开仓库。

本节只证明诊断对照已准备到外置介质。尚未取得本对照的真机启动日志；成功验收仍要求原有 22 字段及精确 PC／参数／清单核对，附加诊断标记不得表示失败。即使 diagnostic return=1 取得日志，也只算诊断返回，不能认定完整平台检查点通过。


## 2026-10-29：诊断返回对照仍黑屏

诊断镜像 SHA-256 `81468498db2ac78c6f9ffbb5bf07b2404c65422bb4d2d3c544cc7f2d37bf67dc` 手动启动后仍黑屏；恢复内置 Linux 后未发现 probe-log，没有取得有效记录。本轮不能确认到达 HVC，亦不能排除捕获／完整页表硬门限或 Linux 交接停止，完整平台返回仍未通过。

下一份 version=8 把边界退到原生 SoC 属性读取及缓存写入完成处，详见 [SoC 属性诊断中点](xnu-soc-checkpoint.md)。106 项主机回归、四种既有探针构建通过，实际内核指令的主机重放到达精确中点；原生结果与 SoC 缓存、PE／DT／CPU 数据、未执行全局和四层活动帧模型一致。主机对照不能代替真机证据。


## 2026-10-29：SoC 属性中点介质准备完成

version=8 镜像 SHA-256 `d8a705a545993f24de32dc81ec42535c6b2c4fa5e22c1e80135f9a79a0f749cb`，签名容器 SHA-256 `524355ae9589785547defaea5f8b67fffe09cdc7d3c0385d08d6fb2b107d1e4c`。最终源码／产物清单、主备 GPT CRC／分区属性和内嵌容器一致；主机与目标端签名／body 校验通过，目标端镜像／容器／读取器传输哈希一致。

重新核对既有可清空 USB／removable、容量及 root／boot／swap 排除后，固定哈希私有写入器独占打开外置介质，完成全部 128 MiB 写入、同步和全量回读，SHA-256 与本轮镜像相同。没有写固件或内置盘，没有自动重启。独立源码／实现／测试／文档审查完成，未留下待修正问题；私有写入器与原始设备数据不进入公开仓库。

本节只证明新介质准备完成，SoC 中点尚未真机验收。手动启动后要求 `read_probe.py --xnu-soc` 的 20 字段、精确 SoC 函数 HVC 边界 +0xb8（日志 ELR 为 +0xbc）与本轮清单匹配；失败诊断记录不能算通过，后续完整平台返回仍未验证。


## 2026-10-29：SoC 属性中点仍黑屏

version=8 镜像 SHA-256 `d8a705a545993f24de32dc81ec42535c6b2c4fa5e22c1e80135f9a79a0f749cb` 手动启动后仍黑屏；恢复内置 Linux 后没有 probe-log。本轮没有有效硬件记录，不能确认已执行完 SoC 属性消费，亦不能区分源代码停止、捕获／页表硬门限或 Linux 交接问题。

连续无日志后，先核对公共返回路径。对照使用当前未修改包装器，装入此前已通过 19 字段硬件验收的完整 version=6 payload，负载逐字节相同；不是把旧包装器一并回退。两个负载的 ADT 输入相同，Linux 与包装器内存布局一致。若通过，可证明当前公共装载／返回路径在旧边界仍可工作，故障仍可能在新增原生执行或边界专属校验；若不通过，优先调查公共路径／环境回归，仍不能仅凭黑屏定位指令。此对照不验证 SoC 属性或完整平台返回，也不放松任何返回门限。


## 2026-10-29：当前包装器与已验收负载的回归对照准备

包装器运行时代码来自 `7ee08e7`，内嵌 version=6 payload 与此前验收文件逐字节相同，SHA-256 `7780d1a4a74a1dba08dc950219fda6e66d3aac9874fcc6661bb263f0589d6725`。当前装载器、数据与捕获模型接受该真实负载；此前私有硬件记录仍通过严格 19 字段读取。没有修改运行时代码或新增负载版本，本轮没有重新运行完整回归。

对照镜像 SHA-256 `638baa7c07100a10c66a292e53435f0f096cf399a1826325eaf37fccda756548`，签名容器 SHA-256 `23e3c824eef987ec9339a609f24294b85623d82e940b755051777f5e015bc685`。独立检查确认当前源码清单、全部产物摘要、原样负载、Linux／命令行、47 份 DTB 和内存布局符合对照要求；实际主备 GPT CRC、分区属性和内嵌容器正确，6611 条指令／13 段满足标量约束。独立审查未发现阻断问题；主机和目标端 vboot 签名／body、传输哈希校验通过。

重新确认可清空外置介质的 USB／removable、容量及 root／boot／swap 排除后，固定哈希私有写入器独占打开介质，完成全部 128 MiB 写入、同步与完整回读，SHA-256 与本对照镜像一致。没有写固件或内置盘，没有自动重启；私有工具和原始设备数据未公开。待手动启动，以 `read_probe.py --xnu-platform`、本轮镜像清单及精确返回 PC 验收；本节只记录准备完成。


## 2026-10-30：当前包装器的 mode6 回归对照通过

运行时代码 `7ee08e7`、对照准备记录 `e15bb0a`，镜像 SHA-256 `638baa7c07100a10c66a292e53435f0f096cf399a1826325eaf37fccda756548`。沿已全量回读的外置介质手动启动流程返回原生 Linux，设备树提供完整 8192 字节日志；`read_probe.py --xnu-platform` 的全部 19 个必需字段唯一且满足预期。日志头、终止和零填充有效，设备属性与私有副本独立摘要一致；没有公开原始日志或其哈希。

独立核对原样 version=6 payload、首次原生查找的精确 HVC／ELR、参数虚实位置与对齐关系，64 字节缓存行 × 393216 行覆盖全部 24 MiB arena；当前源码与全部产物清单、目标端镜像／容器／读取器传输哈希匹配。没有把旧记录的内容差异作为启动新鲜度证明。此次只读取并保存证据，没有重写介质或自动重启。

19 字段确认当前包装器在此前查找边界仍能装载、执行、核对完整页表／硬件 AT、正常 CPU／PE／DT 数据和活动栈帧，完成缓存维护及状态恢复后返回 Linux。由此缩小公共路径回归的疑点；不能推广为 mode7／mode8 的边界专属捕获门或后续执行正常。SoC 属性中点和完整平台返回仍未验收，未知停止位置仍可能在新增原生执行、边界校验或该路径上的交接。

下一步应补足后续路径的原生异常和返回校验失败现场，使无日志的原因可区分；原有成功门限继续独立保留。AP UART 尚未验证，日志诊断能力也不能视作完整平台支持。


## 2026-10-30：同步异常诊断与主动故障对照介质准备

已实现独立 version9 诊断与 version10 主动同步故障对照，详见 [诊断契约](xnu-diagnostics.md)。严格成功检查保留；异常或正常现场不匹配时，只有独立入口／翻译／完整表池／缓存传输门通过才带回失败记录。version10 在此前已验收的首次 ADT 查找边界以单条 BRK 替换原始 MOV，要求精确故障 PC、预期 BRK syndrome 和同步向量 HVC，一律不算 SoC 成功。两处同步向量首条跳转单独审计替换，其他异常不改为成功路径。

9 项新增诊断用例及完整 115 项主机回归、四种既有探针构建与独立设计／实现／文档审查通过。实际源码指令主机重放分别到达 version9 SoC 中点和 version10 的 BRK；前者全局／活动帧及四项数据验证通过，后者触发前模型与已验收 mode6 边界一致。重放没有执行真实硬件异常分派、MMU／缓存或 EL2 恢复，不能代替真机通道验收。实际包装器反汇编、7114 条指令／14 段标量约束、两处向量 HVC／控制 BRK／原有停止 HVC 共四处补丁核对正确。

version10 镜像 SHA-256 `f52001559f29faab8071f731106ae1e0fc3d87b33ee5529eba28241b280f8fa2`，签名容器 SHA-256 `b7c7c06253788d5a8c7b037cdb1b7dada6125ad44e57ae2c9309a31051d2b99b`。最终源码和全部产物清单、实际主备 GPT CRC／分区属性／内嵌容器一致；主机与目标端签名／body 和传输哈希通过。Linux、47 份 DTB、initrd、命令行和 74 MiB 布局保留。

重新确认既有可清空外置介质的 USB／removable、容量及 root／boot／swap 排除后，固定哈希私有写入器独占打开介质，完成全部 128 MiB 写入、同步和完整回读，SHA-256 与本 version10 镜像一致。没有写固件或内置盘，没有自动重启；原始设备数据与私有工具未公开。

本节只证明主动异常对照介质已准备。手动启动后以 `read_probe.py --xnu-diagnostic` 和本轮清单、精确预期 BRK 现场验收；预期 reason=1、fault received=1、checkpoint=0、diagnostic return=1 和传输／页表结果通过，只算异常记录回收。后续 version9 真实故障诊断与 SoC／完整平台成功均仍待真机证据。


## 2026-10-30：主动故障对照首轮仍黑屏，定位包装器缺陷

运行时代码 `8e98a9b`，上一节已回读镜像 SHA-256 `f52001559f29faab8071f731106ae1e0fc3d87b33ee5529eba28241b280f8fa2`。手动尝试后仍持续黑屏；恢复内置 Linux 后没有设备树 probe-log。异常通道未验收，无法从黑屏确定实际执行至何处；本次只读取设备状态，没有重写介质或自动重启。

补充 TCG 完整 CPU／异常级模型后，原包装器在进入 XNU 前的 ADT 契约检查拒绝真实输入。原始对象与已发出 ELF 的独立检查确认字符串指针聚合保留零链接地址：PE 解析器有 34 个已分配节 ABS64，故障标签有 4 个。该确定缺陷自 mode7 的装载前路径生效；mode6 绕过它。修复改用内联字符数组，原 XNU 负载、停止点与成功门限不变。详情见 [生成码与系统回归说明](xnu-diagnostics.md)。

118 项主机回归、四种探针构建、链接前逐 C 对象审计和独立审查通过。系统模型实际运行原包装器入口、装载器、源码 XNU、MMU 指令与异常返回；mode6 严格 19 字段、version9 严格 20 字段成功，version10 精确 BRK 故障记录及诊断传输通过，后者还在第二个非零加载位置复验。板级 FDT 准入、日志导出与 Linux 入口由桩代替；TCG 缓存指令／计数不证明物理缓存行为。这些结果修复主机验证缺口，不代替真机日志，也不能据此认定此前每轮黑屏均已解释。


## 2026-10-30：重定位修复后的同负载故障对照介质准备

外层 C 包装器修复四处字符串聚合，version10 XNU payload 与首轮逐字节相同，SHA-256 `8df4b024beb7ca8a45f300de92f56a662d732e1afbd8972c0431dd8db17a7d38`。修复镜像 SHA-256 `9bfd2071c48fa77c8f079fec42c1eaf196881001d9f00ee3e2460b5a93d1e7f4`，签名容器 SHA-256 `2e5575ffab33266830f6c60653ff8b360f50a169c100620676294d05f8520cd3`。最终 ELF 导出字节与 Image 一致；12 个实际 C 对象的已分配节审计通过，7142 条指令／14 段满足标量约束，74 MiB 内存布局及 Linux／47 份 DTB／initrd／命令行保留。源码与产物清单、实际主备 GPT CRC、分区属性和内嵌容器独立核对通过，主机与目标端 vboot 签名／body 以及传输摘要一致。

再次核对既有可清空外置 USB 的 removable／容量，排除当前 root／boot／swap 后，以固定镜像哈希的私有写入器独占打开介质。全部 128 MiB 写入、同步与完整回读完成，摘要与修复镜像一致。没有写固件或内置盘，没有自动重启；没有公开原始设备数据、私有地址或写入工具。

本节仅证明修复介质准备完成。待手动启动后，用 `read_probe.py --xnu-diagnostic` 和本轮清单核对精确 BRK 现场，预期仍为 reason=1、fault received=1、checkpoint=0、diagnostic return=1，传输与完整页表门通过。此对照的真机异常回收以及后续 version9／完整平台成功均尚未验收。


## 2026-10-30：version10 主动同步异常通道真机通过

运行时代码与准备记录 `93c9e22`，镜像 SHA-256 `9bfd2071c48fa77c8f079fec42c1eaf196881001d9f00ee3e2460b5a93d1e7f4`。沿已全量回读的外置介质手动启动后返回原生 Linux，设备树提供完整 8192 字节 probe-log。日志头、终止符与零填充有效，严格诊断读取器的 46 个必需字段唯一且一致；设备属性与私有副本的独立重复读取摘要匹配，没有公开原始记录或其摘要。目标端保存的镜像／容器／读取器与本轮清单和主机文件一致。

精确 BRK 指令与故障 PC、预期 ESR、SP0 同步向量 HVC、原始 EL1t 和捕获 EL1h 状态相符。结果 reason=1、fault received=1、diagnostic transport verified=1、table pool verified=1；64 字节缓存行 × 393216 行覆盖完整 24 MiB arena。独立检查确认捕获的虚实内存布局、参数／页表位置、TCR／MAIR／VBAR、AT 结果和边界栈状态与该清单／负载一致，状态恢复后完成原生 Linux 交接。

checkpoint=0、diagnostic return=1、capture verified=0 符合主动故障对照；普通 CPU／PE／SoC 成功字段不会因故障传输而通过。因此本轮仅验收同步异常回收通道，不验收 SoC 属性或完整平台函数，也不据此确定此前每轮黑屏的精确停止位置。读取并保存本轮证据时没有写介质或自动重启；下一份 version9 的介质准备另外记录。


## 2026-10-30：version9 实际 SoC 路径诊断介质准备

在 version10 主动异常通道验收后，准备 version9，继续原始 SoC 属性读取／缓存写入路径，在既定 SoC 中点返回。未修改 `93c9e22` 的运行时代码；实际 Image 在负载区之外与本轮已验收 version10 逐字节相同。负载仅变更版本标签、把主动 BRK 恢复为原 MOV，保留两处同步向量 HVC 与原 SoC 中点 HVC；当前打包器从固定源码／ADT 重建出的字节与输入一致。该 version9 已有相同源码和负载的 TCG 严格 20 字段结果，仍只算主机证据。没有运行时代码或测试变更，本次未重复运行此前通过的 118 项回归。

version9 payload SHA-256 `4a105ea36986bdeeb5a7b91acc7d29237d7231fe774130f1db1673792a5560b4`，镜像 SHA-256 `649fcee53bb78b4eb3c7d62a2fefb770bd58b2c4c4d62ee3eda29965b4950c58`，签名容器 SHA-256 `6a49aba9e1526501bc72063c365aa60763e4098ba4bb67abf8f8def71b73cbdc`。最终源码／产物清单、ELF 导出字节、完整内嵌负载、12 个实际 C 对象的重定位审计、7142 条指令／14 段标量约束、74 MiB 内存布局、Linux／47 份 DTB／initrd／命令行、实际主备 GPT CRC／分区属性／容器及独立审查均通过。

主机与目标端 vboot 签名／body 和传输摘要验证通过。再次确认既有可清空 USB 的 removable／容量并排除 root／boot／swap 后，以固定哈希私有写入器独占打开介质，完成全部 128 MiB 写入、同步与完整回读，摘要与本 version9 镜像一致。没有写固件或内置盘，没有自动重启。

本节仅证明 version9 介质准备完成，尚未取得其真机运行记录。手动启动后的成功要求严格诊断读取通过，并同时满足全部 20 个 SoC 字段、reason=0、checkpoint=1、fault received=0、diagnostic return=0 及清单／精确停止点核对；其他诊断结果只表示回收失败现场。完整平台返回和完整系统启动继续未验收。


## 2026-10-30：version9 SoC 属性中点真机通过

运行时代码 `93c9e22`、准备记录 `3e72b91`，镜像 SHA-256 `649fcee53bb78b4eb3c7d62a2fefb770bd58b2c4c4d62ee3eda29965b4950c58`。沿已全量回读介质手动启动后，原生 Linux 收到完整 8192 字节日志，头／终止符／零填充有效；46 个诊断必需字段及全部 20 个严格 SoC 字段唯一且通过。实时设备树属性与私有副本的独立重复读取摘要一致，目标保存的镜像／容器／读取器和主机清单匹配；没有公开原始记录或其摘要。

version9 的结果为 reason=0、checkpoint=1、fault received=0、capture verified=1、table pool verified=1、diagnostic transport verified=1、diagnostic return=0，四个原始异常寄存器为零。精确 SoC 中点 HVC、原始 ADT 边界 MOV、参数／CPU／PE／DT／SoC 数据及四层活动帧符合预期；独立核对 SoC 基址与真实 ADT ranges、TCR／MAIR／VBAR／页表、虚实地址与硬件 AT、栈状态和返回地址，缓存行 64 字节 × 393216 行覆盖完整 24 MiB arena。恢复状态后成功交接原生 Linux。

这确认原生 SoC 属性读取与缓存写入、完整验证及正常返回，没有访问 SoC MMIO；SoC 中点之后的 CPU／时钟／型号／调试工作、完整 PE 返回和设备支持仍需分别验证。历史 version8 黑屏尝试不据此改记成功。保存证据期间没有写介质或自动重启；后续完整 PE 的介质准备另外记录。


## 2026-10-30：修复包装器后的完整 PE 返回介质准备

在 version9 SoC 属性中点真机验收后，回到此前已审计的 version7 完整 PE 返回负载。原 payload SHA-256 `b2e73dfbfdae5ce5033ea8bf24de37ef0981a9fff00315e5b6ffda5d0775683f` 与首轮逐字节相同，当前打包器从固定源码／ADT 重新生成的字节也完全一致；运行时 C 代码沿用 `93c9e22` 的修复，不增加执行阶段或放松返回门。保留原生 EL1 同步向量，停止于完整 PE_init_platform(FALSE) 的原有返回点，尚不进入 timer／设备初始化。

主机系统工具只增加 version7 的严格 22 字段读取分支。完整 PE 的 22 字段与 mode6／9／10 系统 CPU 模型回归均通过，独立核对报告与当前源码、输入负载匹配；板级准入／日志导出／Linux 入口为桩，物理缓存与真机结果继续单独验收。本轮没有修改运行时代码或普通回归用例，未重复此前通过的 118 项完整测试。

镜像 SHA-256 `a4453bb9c721dd26e6d7035a1459dc7c0e9b8753e4c629a4354e7ced49278046`，签名容器 SHA-256 `3a6393dba5708dca5a246d5886617b98411981c4194a1038b04bcd21045e5dff`。最终源码／产物摘要、ELF 导出与 Image、原样完整负载、12 个 C 对象重定位审计、7142 条指令／14 段标量约束、74 MiB 内存布局、Linux／47 份 DTB／initrd／命令行、实际主备 GPT CRC／分区属性／容器及独立审查通过。主机与目标端 vboot 签名／body、传输摘要一致。

再次核对既有可清空 USB 的 removable／容量，排除 root／boot／swap 后，以固定哈希私有写入器独占打开介质，完成全部 128 MiB 写入、同步与完整回读，摘要与本镜像一致。没有写固件或内置盘，没有自动重启。

本节仅证明完整 PE 介质准备完成，尚未取得本镜像的真机运行日志。成功须通过 `read_probe.py --xnu-pe` 的全部 22 字段及本轮清单、精确返回 PC／参数核对；失败诊断不能算完整平台通过。当前缺失频率仍按受限 ADT 合约保留为零，clock info 字段不表示硬件时钟已配置，完整系统启动也仍未验收。


## 2026-10-30：version7 完整早期 PE 返回真机通过

运行时代码 `93c9e22`、准备记录 `c1adc3c`，镜像 SHA-256 `a4453bb9c721dd26e6d7035a1459dc7c0e9b8753e4c629a4354e7ced49278046`。沿已完整回读的外置介质手动启动，原生 Linux 收到完整 8192 字节 probe-log；日志头／终止符／零填充有效，全部 22 个 PE 必需字段唯一且通过。实时属性与私有副本重复读取摘要匹配；目标保存的镜像／签名容器／读取器与本轮清单一致。原始记录及其摘要继续私有。

原样 version7 payload 的完整 PE 返回 HVC／ELR、参数虚实关系和自有 arena 对齐布局匹配；全部 CPU／PE／DT／SoC／时钟结构／型号／调试配置／活动父帧、完整表池和硬件 AT 验证通过，64 字节缓存行 × 393216 行覆盖全部 24 MiB arena。checkpoint=1、return boundary verified=1、diagnostic return=0，恢复状态后完成原生 Linux 交接。独立核对支持这次受限 PE_init_platform(FALSE) 调用成功，不包含 version9／10 的原始异常 46 字段。

缺失频率依旧按当前 ADT 合约为零，clock info 不是时钟驱动验收；timer／WFE、CPU 拓扑／线程／VM、中断／SMP 和完整系统启动仍未验证。此前无日志轮次仍无法确定精确停止位置。此次仅读取／保存证据，没有改写介质或自动重启，也没有生成新的真机测试镜像；下一阶段的源码与生成码依赖见 [后续审计](post-pe-audit.md)。本轮未修改运行时代码或测试，没有重复运行已有主机回归。


## 2026-10-31：version11 WFE／单 CPU 拓扑主机验证及介质准备

独立 version11 的执行／输入／恢复契约见 [说明](xnu-topology-checkpoint.md)。保留原生同步向量，在拓扑解析返回后、下一条 ml_get_boot_cpu_number 调用前以单条 HVC 返回；核对全部原 PE 数据、完整拓扑对象／未用槽／父帧、WFE 值和 CNTKCTL／TPIDR 对入口／运行／恢复关系，不进入线程或 VM。

126 项完整主机测试、四种基础探针构建通过。七次 TCG 系统模型回归使用实际包装器、原生 XNU 与 MMU／HVC 返回，三次 version11 覆盖零值／不同非零系统寄存器初值和不同加载地址，旧 mode6／7／9／10 各自严格回归。version11 全部 40 字段通过，Linux 入口桩另行读取并核对三个恢复寄存器；模型桩和虚拟缓存不代替板级准入、真实 Linux 启动或 SC7180 缓存证据。

设计提交 `cfd8b2f`，运行时提交 `3d6acfd`。镜像 SHA-256 `83786588e6d8c06b3256eb253bfcb065edaf35578b08ec734f0442ff1060409a`，签名容器 SHA-256 `4b6caa9a288dd536b9c2c84c0ecc0bcc688da7cb4babffb73962f87502a40e55`，负载 SHA-256 `23457b14fc4d9971237f05e356015eb2ed8415a424f68dfc7544c801715c76b7`。源与产物摘要、ELF 导出／Image、原样负载、13 个实际 C 对象重定位审计、7883 条指令／15 段标量约束、74 MiB 布局、保留 Linux／47 份 DTB／initrd／配置／命令行、实际主备 GPT CRC／属性／容器、主机 vboot 签名／body 与独立审查通过。

目标连接恢复后，独立比较镜像／签名容器／清单／读取器／公开密钥／私有写入器全部传输摘要，目标端 vboot 签名与 body 验证通过。再次确认既有可清空外置 USB 的 removable／容量及 root／boot／swap 排除后，固定哈希私有写入器独占打开该介质，完成全部 128 MiB 写入、同步与完整回读，摘要与本镜像一致。没有写固件或内置盘，没有自动重启。

version11 真机执行验收仍待进行，必须取得本轮精确边界和 `read_probe.py --xnu-topology` 全部 40 字段；已有完整 PE 硬件通过不会自动升级为拓扑通过。本轮没有加入显示 MMIO／EC 写入或改变 Video 输入；显示复用仅完成 [独立源码审计](display-reuse-audit.md)。


## 2026-10-31：version11 WFE／单 CPU 拓扑真机通过

运行时 `3d6acfd`、准备记录 `2fa1a00`，镜像 SHA-256 `83786588e6d8c06b3256eb253bfcb065edaf35578b08ec734f0442ff1060409a`。沿已全量回读的 USB 手动启动，原生 Linux 收到完整 8192 字节日志；日志头、终止及零填充有效，严格 `--xnu-topology` 的全部 40 个必需字段唯一且通过。两次实时读取与私有副本字节匹配，目标镜像／签名容器／清单／读取器摘要与本轮产物一致。原始日志和摘要保持私有。

独立核对负载 version11、精确 `0xfffffff0079b3d50` 返回 ELR、参数虚实关系和自有 arena／Image 对齐、affinity 与 19.2 MHz timebase。完整 CPU／PE／DT／SoC／型号／时钟结构／调试配置／拓扑对象及未用槽／页大小／stall／活动父帧、完整表池和硬件 AT 通过，64 字节缓存行 × 393216 行覆盖全部 24 MiB arena。checkpoint=1、return boundary verified=1、diagnostic return=0，恢复状态后交接原生 Linux。

WFE EVENTI=4，CNTKCTL 实际运行值符合当次入口值 OR 0x4f；两个 TPIDR 运行值符合唯一逻辑 CPU／cluster，交接前三个系统寄存器的实际回读均与入口一致。本次三个入口值均为零；不同非零初值的恢复仍是主机系统模型证据。该轮验证的是 WFE 配置与单 CPU 拓扑解析，不含睡眠唤醒、计时器中断、线程／VM 或其他核启动。缺失频率与缓存／PIO 属性仍按受限 ADT 缺省模型处理，不能据字段通过宣称设备驱动已支持。

本轮仅读取并私下保存日志、更新脱敏记录；没有改写运行时、测试介质、固件或内置盘，没有自动重启。未修改代码或测试，不重复此前通过的 126 项主机回归。显示恢复仍只有独立源码审计，下一步优先限定 [显示探针范围](display-reuse-audit.md)。


## 2026-10-31：独立只读显示发现介质准备

在 version11 拓扑真机通过后，按 [显示发现契约](display-discovery.md) 实现独立不携带 XNU 的原生探针。Coreboot 表校验与 framebuffer／backlight 元数据由有界解析器处理，只读快照 13 个固定显示／GPIO／EC 协议寄存器；受保护的精确 EL1h 读异常可返回失败，其他异常不改为成功。Coreboot 表 RAM 读取仍依赖原厂 FDT 的 CBMEM 来源，不能把 MMIO 保护推广到任意表指针。完整帧与 4 KiB 分配跨度、1 MiB 基址、Coreboot／FDT／Image 排除独立核对；候选标志不授权写入或证明可见。

137 项完整主机测试通过，含新增 11 项解析／状态／读取器／实际 C FDT 路径用例；主机 C 路径的表获取与 MMIO 使用桩。11 次 TCG 验证读取成功、真实 data abort／出参不变／异常寄存器及 callee-saved GPR 恢复、HVC／Linux 入口桩与九种精确门限拒绝；两种加载地址通过，不含 SC7180 MMIO、真实 Linux 或显示操作。四种基础构建及当前源码下旧 version11 的严格 40 字段 CPU 模型回归通过。

运行时 `abac621`，设计 `55e6f66`。镜像 SHA-256 `07a4272f4e18488502f000eee3bfd644f054af634407069187f956bc2f014b29`，签名容器 SHA-256 `1f89f5a8660b9f22160e1a8bbbb454182d47ce1aa334fefb22aa868c75a0f3ca`。七个实际 C 对象重定位审计、4206 条指令／9 段标量约束、精确受保护 LDR、最终 Image 中 13 项白名单、源码／产物摘要、ELF 导出、保留的 Linux／47 份 DTB／initrd／配置／命令行、主备 GPT CRC／属性／容器及独立审查通过。

目标端镜像／容器／清单／两个读取器模块／公开密钥／私有固定哈希写入器传输摘要匹配，vboot 签名／body 验证通过。再次排除 root／boot／swap 并核对既有可清空外置 USB 的 removable／容量后，独占写入全部 128 MiB，完成同步及完整回读，摘要与镜像一致。没有写固件或内置盘，没有自动重启。

原生发现尚未验收，必须手动启动后取得 `read_probe.py --display-discovery` 的完整 42 字段；不完整或异常返回仅为失败证据。当前镜像不写 framebuffer、显示控制、GPIO 或 EC，不发 SPI 事务，也不点亮屏幕。后续先据真实 scanout／保留状态／冲突记录限定图案与背光实验；原始表、物理地址、日志及其摘要保持私有。


## 2026-10-31：独立只读显示发现真机验收

运行时 `abac621`、准备记录 `476ce2e`，使用前述同一镜像与签名容器。目标保存的镜像／容器／清单／两个读取器模块与本地最终产物一致；清单的源码及产物摘要另行重算核对。原生 Linux 收到完整 8192 字节 probe-log，两次独立实时读取字节一致，私有副本通过当时的 `read_probe.py --display-discovery`（现为 `--display-discovery-schema1`）全部 42 个严格必需字段。入口 EL2、探针 EL1、stage=3、交接 EL2 均匹配。

Coreboot 表与 framebuffer／GPIO 元数据有效，13 次固定 MMIO 读取完成、故障为零、硬件写入为零，随后正常交接 Linux。TE 与 DSI control 均为零，与源码停机路径相符；本轮不推断其余时钟、PHY、PLL、面板或 SMMU 状态，也没有执行 XNU、SPI 事务、framebuffer 或背光操作。

几何、XY、stride、format、unpack 与有效元数据逐项匹配；原始表 PA 为零，实际 scanout 来自 SSPP。Coreboot RAM 判定为真，与当前 Image 的完整分配范围不冲突。但实际 scanout 仅满足 4 KiB、未满足当前候选的 1 MiB 基址对齐，故现有“scanout consistent”合取结果为假；这不能误写为几何或像素格式错误。FDT 范围门也返回假：复用的 `xnu_range_allowed()` 整体排除固件加载与运行区，当前 scanout 落在该排除带内。该结论并不能单独说明它与某个具体活跃拥有者冲突，仍须检查当次 FDT／Coreboot 分配和完整保留范围。

最终可写候选为假，严格读取器正确接受“只读发现完成、候选不可用”。当时把 1 MiB 基址当作锁定分支的动态分配行为；下一节核实确认这是误读。不能先放宽门限再直接绘制。原始物理地址、表、日志及其摘要保持私有。

本轮仅读取、保存证据并更新脱敏记录，没有改写运行时或测试介质，没有写固件或内置盘，没有自动重启；无代码变更，不重复已通过的 137 项测试。该结果验收发现与 Linux 返回，不验收图案可见、背光、XNU Video 或完整系统启动。


## 2026-10-31：真实 scanout 分配与归属只读核实

没有新的启动。在产生上述记录的同一次启动中，只读取设备树；其中的 probe-log 与私有记录逐字节相同。未使用 sudo，未写入设备、USB、固件或内置盘，没有自动重启。

候选为假的两个原因均已查明。1 MiB 基址门误读了锁定 libpayload：分配器对齐的是末端，基址只到 4 KiB；按该逻辑重放私有内存表的结果与真机 SSPP 基址逐位相等。整体排除带必然覆盖动态分配的 framebuffer，这道门对该对象不可能为真；仅移除该项的临时副本在重建 FDT 上返回真，八组反例符合预期，这是主机结果。

当次 `/memory` 覆盖完整分配范围，reserved-memory 节点都不与它重叠，已识别的拥有者只有 Depthcharge／libpayload 遗留的旧分配。当次 memreserve 未直接读取，已安装固件二进制未比对，SMMU、MDP、缓存及其他 DMA 主设备没有证据；没有绘图、背光或 EC 事务，也没有可见画面记录。完整证据与后续字段见 [分配与归属核实](display-discovery.md#2026-10-31真实分配与保留归属核实)。


## 2026-10-31：schema 2 显示发现介质准备

运行时 `9ae6150`。镜像 SHA-256 `b70314a11b6fe4bb062df17f4c285e0fdd5e5b4d38e2b3f4bc895350a3d151bd`，签名容器 SHA-256 `4a31c10a2cf8d826fb8bb58d3083ade2bc38ce46528be356d026051636902c80`。相对上一张发现镜像只换包装器，Linux、47 份 DTB、initrd、配置与命令行不变；原生 Image 的 4250 条指令／9 段标量审计、7 个 C 对象重定位审计及主备 GPT 核对通过。目标端传输摘要与 vboot 签名／body 验证通过。

写入器重新核对 removable USB、容量及 root／boot／swap 排除后，完成全部 128 MiB 写入、同步与完整回读，摘要与镜像一致。没有写固件或内置盘，没有自动重启。

本节只证明介质准备完成，schema 2 尚无真机记录。手动启动后屏幕仍不点亮；回到 Linux 后用 `read_probe.py --display-discovery` 验收全部 45 字段。候选为真也不授权写入，不代表屏幕可见。


## 2026-10-31：schema 2 显示发现真机验收

运行时 `9ae6150`、准备记录 `3dd81fb`，使用前述同一镜像。手动启动后原生 Linux 收到完整 8192 字节 probe-log，两次实时读取字节一致，目标端与本地 `read_probe.py --display-discovery` 都通过全部 45 个严格字段。入口 EL2、探针 EL1、stage=3、交接 EL2 均匹配；13 次读取、零故障、零硬件写入，随后正常交接 Linux。

元数据一致、分配关系、分配窗口、Coreboot RAM、FDT 拥有者、显示归属与最终候选均为真，Image 冲突为假。范围门这次运行在当次启动的原始 FDT 上，补上了上一轮 memreserve 未直接读取的缺口。scanout、分配跨度、几何及其余寄存器读数与上一次启动相同，TE／DSI 仍为零。

候选为真只说明只读判断通过。本轮没有写 framebuffer、显示控制、GPIO 或 EC，屏幕未点亮，没有可见画面记录。已安装固件二进制、SMMU、MDP、缓存及其他 DMA 主设备仍未证实。原始日志、地址及其摘要保持私有。


## 2026-10-31：第一次画面实验介质准备

运行时 `4f2202a`。镜像 SHA-256 `21f17f79c8934a45de293d4164fd58b670c28261c2d7f76fd91034dc93c1019c`，签名容器 SHA-256 `210270dbffb42132c791305ed690fe5de1f2224696cd86bba2199a5706f825c8`。4605 条指令／9 段标量审计、7 个 C 对象重定位审计及 GPT／容器核对通过，Linux、47 份 DTB、initrd、配置与命令行不变。目标端传输摘要与 vboot 验证通过。

写入器重新核对 removable USB、容量及 root／boot／swap 排除后，完成全部 128 MiB 写入、同步与完整回读，摘要一致。没有写固件或内置盘，没有自动重启。尚无这张镜像的真机运行记录；回到 Linux 后用 `read_probe.py --display-pattern` 读取，肉眼现象单独记录。


## 2026-10-31：第一次画面实验真机通过

运行时 `4f2202a`、准备记录 `337d7e5`，使用前述同一镜像。手动启动后屏幕出现全屏棋盘格，与写入的图案一致，这是本路线第一次在交接后恢复出画面。没有开背光，需要手电照射才能看到，与背光仍关闭的预期一致；格数未逐一清点。

回到原生 Linux 后读取完整 8192 字节 probe-log，两次实时读取字节一致，`read_probe.py --display-pattern` 通过全部 58 字段。入口 EL2、探针 EL1、stage=3、交接 EL2 均匹配；13 次只读 MMIO 零故障，全帧回读零差异，7 次寄存器写入后 TE=1、DSI control=`0x1f3`，停止后两者均读回零。

INTF 帧计数在 19 秒内增加 1140，换算正好 60.00 Hz，确认时序引擎真实运行，也确认了取自 Linux 的计数偏移在这块硬件上有效。起始计数不为零，原因未查明，这里只使用差值。

本轮只恢复 scanout。没有写 EC、GPIO 或背光，没有重新初始化面板或桥接芯片；入口 GPIO 读数仍为零。XNU、字符输出和背光控制不在本轮范围，完整系统启动仍未验收。原始日志、地址及其摘要保持私有。


## 2026-10-31：背光实验介质准备

运行时 `daf9817`。镜像 SHA-256 `d1256b8a918db4ef919b234033e5dabeacc9a115041060e8cf6246b9cbb0ad29`，签名容器 SHA-256 `44bfeb34c9b4e4e08630883322ac4c11282705bc04ffac5eaaae70278b10da9b`。5120 条指令／10 段标量审计、8 个 C 对象重定位审计及 GPT／容器核对通过，Linux、47 份 DTB、initrd、配置与命令行不变。目标端传输摘要与 vboot 验证通过。

写入器重新核对 removable USB、容量及 root／boot／swap 排除后，完成全部 128 MiB 写入、同步与完整回读，摘要一致。没有写固件、EC 固件或内置盘，没有自动重启。尚无这张镜像的真机运行记录；回到 Linux 后用 `read_probe.py --display-backlight` 读取，肉眼现象单独记录。


## 2026-10-31：背光实验真机通过

运行时 `daf9817`、准备记录 `9625912`，使用前述同一镜像。手动启动后屏幕在不用手电的情况下亮起，显示横 8 格、竖 6 格的黑白棋盘格，左上角为黑格，与写入图案的格数和方向一致，保持结束后熄灭并进入原生 Linux。这是本路线第一次在交接后恢复带背光的可见画面。

Linux 收到完整 8192 字节 probe-log，两次实时读取字节一致，`read_probe.py --display-backlight` 通过全部 64 字段。入口 EL2、探针 EL1、stage=3、交接 EL2 均匹配；13 次只读 MMIO 零故障，全帧回读零差异。

EC 协议 3 在第一次运行就工作：`EC_CMD_HELLO` 回复校验通过，`EC_CMD_PWM_SET_DUTY` 的开和关都成功，错误码为零。背光 GPIO 输出读回为高，结束后读回为零。9 次寄存器写入后，TE 与 DSI control 均读回零；帧计数 19 秒增加 1140，仍为 60.00 Hz。

只发送了 EC 运行状态命令，没有写 EC 固件、闪存或内置盘，也没有重新初始化面板或桥接芯片。XNU、字符输出不在本轮范围，完整系统启动仍未验收。原始日志、地址及其摘要保持私有。


## 2026-10-31：XNU 屏幕输出介质准备

运行时 `9342745`。镜像 SHA-256 `331b4c4256f2c87c317ae23a23cda66c8555a175e41b08310f5528a77dd6b7ac`，签名容器 SHA-256 `cbcff75e8e6962b5fa3325a2dd680802388e459c10e08780d3c6d737ad733b03`。内核由 `platform/coachz-screen` 单独构建，ADT 与已验收的 version11 负载相同。10645 条指令／20 段标量审计、18 个 C 对象重定位审计通过。目标端传输摘要与 vboot 验证通过。

写入器重新核对 removable USB、容量及 root／boot／swap 排除后，完成全部 128 MiB 写入、同步与完整回读，摘要一致。没有写固件或内置盘，没有自动重启。尚无真机运行记录；这张镜像进入 XNU 后不回 Linux，结果看屏幕。


## 2026-10-31：XNU 第一次屏幕输出

运行时 `9342745`、准备记录 `eadbb38`，使用前述同一镜像。手动启动后屏幕亮起，XNU 把启动过程直接画到屏幕上，结果来自屏幕照片，这张镜像不回 Linux，没有日志记录。

屏幕依次显示 `arm_init`、`cpu_data_init`、`PE_init_platform`、`ml_parse_cpu_topology`、`cpu_bootstrap`、`kernel_startup_bootstrap`、`cpu_init`、`arm_vm_init`（含屏幕别名映射、关闭 TTBR0、`pmap_bootstrap`）的完成标记，随后出现 `kprintf initialized` 和 `Serial mode specified`。这是 XNU 第一次在真机上越过 version11 的停止点并完成 VM 初始化，不再有检查点返回或逐字段核对，这些阶段的证据只是屏幕上的进度。

随后内核 panic 并完整显示现场：`Kernel data abort`，PC `0xfffffff007949f74`（`PE_create_console+0x48`，即内联的 `check_for_panic_log`），ESR `0x96000010`（同步外部中止）。当时 `x1` 为 `0x3c0fc000`，是 BCM2837 机器配置固定的 `PLATFORM_PANIC_LOG_PADDR`，在 SC7180 上不是可访问的内存。与 `boot_args.Video` 无关。

修正：在 `platform/coachz-screen` 启用 UniXNU 现成的 `CONFIG_XNU_DT_PANIC_REGION`，改从设备树读取 panic 区域。完整系统启动仍未验收，组件 prelink 仍未接入。


## 2026-10-31：XNU 屏幕输出越过 panic 区域问题

运行时 `c807a9a`，镜像 SHA-256 `3cec4347877c4a94e718c79c3432d1c3a889c77b0ee97338f2f278864b528006`。启用设备树 panic 区域后，上一处外部中止消失。屏幕照片显示 `zone_init` 完成全部映射，并打印 `VM bootstrap done`，这是 XNU 第一次在真机上完成 VM 引导。

随后内核 panic：`kalloc.c:1779 Assertion failed: n_zones_sig + n_zones_type + 1 <= 32`。这是 DEVELOPMENT 构建的断言。该函数按内核内类型统计把区域预算分给各大小类，默认预算 260；未接入组件 prelink 时类型分布偏向少数大小类，某一类超过 32。

修正：在 `tools/xnu_screen.py` 的命令行加入现成的 `kt_fixed_zones=120`，无需改内核。接入 prelink 后应重新评估这个值。这些都只有屏幕证据，完整系统启动仍未验收。

构建时发现另一处问题：编译缓存不跟踪 `.incbin`，同一输出路径可能复用旧负载。`linux_log.py` 现在把负载摘要写入汇编文件，并核对镜像确实包含该负载。


## 2026-10-31：XNU 屏幕输出进入 IOKit

运行时 `099c601`，镜像 SHA-256 `15012d323cdaf4c5227c875177b832bdac1f9376408abcb5e736b99b06423732`。`kt_fixed_zones=120` 后 kalloc 断言消失。屏幕照片显示调度器、firehose、遥测、BANK 与串行 KDP 初始化，随后进入 `PE_init_iokit`。

panic 的调用栈为 `PE_init_iokit → InitIOKit → OSlibkernInit → OSRuntimeInitializeCPP → OSMetaClass::postModLoad`：`Kernel data abort`，ESR `0x96000006`（转换错误），PC `0xfffffff007834534`，正在沿父类链读取，地址在内核范围之外。静态扫描全部 C++ 静态构造后，只有 `__GLOBAL__sub_I_NativeAES.cpp` 的父类指针超出内核范围，且与现场寄存器值相同。SoftwareAES 的父类 `IOAESAccelerator` 属于只有 prelink 才能提供的组件。此前一轮同一阶段的 `not in the expected zone` panic 尚未单独解释，可能同源。

修正：`platform/coachz-screen` 关闭 `CONFIG_XNU_NATIVE_AES`，接入 prelink 时再恢复。这些都只有屏幕证据，完整系统启动仍未验收。


## 2026-10-31：XNU 屏幕输出到达等待根设备

运行时与配置见随本节提交，镜像 SHA-256 `2a41ced24e468ca9b4a31ab79b2ab886a9c0c6025ca48f48e5a0941955dfa7aa`。这一轮之前依次处理了三处依赖 prelink 的问题：关闭 `CONFIG_XNU_NATIVE_AES`；`CONFIG_XNU_CRYPTO_BOOTSTRAP` 改为 `source`，由内置 corecrypto 提供 `g_crypto_funcs`（否则 `kmem_crypto_init` 空指针）；关闭 `CONFIG_XNU_TRUST_CACHE_RUNTIME`（否则 panic `image4 interface not available`）。

屏幕照片显示 corecrypto FIPS 自检 `all tests PASSED`，IOKit 与 NVRAM 初始化，BSD 版权行，MAC Framework、mbuf、DLIL 和 Skywalk 自检通过，`bsd_autoconf` 调用 `kmInit`，最后打印 `Waiting on <dict>...IOMedia...Apple_HFS...</dict>`，没有 panic。这是 XNU 第一次在 Coachz 真机上完成内核初始化并开始等待根设备。

这只有屏幕证据。没有根文件系统、存储驱动、用户态、完整组件 prelink 或 iPadOS 启动。尚未确认计时器中断是否工作：XNU 约 60 秒后应打印 `Still waiting for root device`，这一轮未记录是否出现。

未解决：早期轮次在 `preModLoad` 的首次 `kalloc` 处随机出现 `not in the expected zone ... found in [0]`，后续轮次未复现，原因未查明。`coachz-screen-console.patch` 暂时保留 `COACHZ META` 诊断输出，以便再次出现时取证。


## 2026-10-31：第一个用户态程序在 Coachz 上运行

运行时与配置见随本节提交，镜像 SHA-256 `526147c61b275f6324f05582e9e2c2646a3fb2b02ab5d782c5bce5b409dd7e84`。这一轮在屏幕镜像上增加了内存盘和 `rd=md0`，`IOService.cpp` 跳过了依赖 IOStorageFamily 的两处断言，并注册了内置 AMFI 接口。

屏幕照片依次显示：`Added memory device md0/rmd0`、`BSD root: md0, major 1, minor 0`、`load_init_program: attempting to load /sbin/launchd`，然后是 `*** COACHZ userspace: init running as PID 1 ***`。这是第一次在 Coachz 真机上离开内核进入 EL0。这行文字由程序自己通过 `open("/dev/console")`、`getpid` 和 `write` 打印，因此这几个系统调用、devfs、mockfs、Mach-O 加载和内存盘读取都走通了。

这只有屏幕证据，没有逐字段核对。运行的是自写的静态测试程序，不是 launchd，也没有 dyld、共享缓存或任何 iPadOS 用户态。`Failed to open swap file` 符合预期，内存盘根上没有可写文件系统。

未确认：计时器中断。程序每 5 秒用 `select` 睡眠后应打印 `COACHZ init: timer tick N`，这张照片里 PID 那行之后还没有 tick，但拍摄时刻距启动多久未知，不能据此判断。`zone` 随机 panic 这一轮未出现，原因仍未查明。


## 2026-10-31：定时器中断在真机上工作

运行时与配置见随本节提交，镜像 SHA-256 `e0960fd1de0dbe69ff0294c23a06c8b9f3fe0f6a1c7592eed5e90f5f0cd29aeb`。

上一节的镜像停在 PID 1 那行，等了很久也没有 tick。真机设备树 `/timer` 写明虚拟定时器是 PPI 3，也就是 INTID 19，内核也确实使用虚拟定时器，而 ArmCpu 只使能并识别写死的 INTID 27，所以定时器中断从未到达内核。`platform/coachz-screen/patches/drivers/ArmCpu.patch` 改为 19，并增加两行屏幕诊断。

屏幕照片依次显示 `COACHZ GIC: IGROUPR0=0 ISENABLER0=87eff GICD_CTLR=12 PMR=f0 IGRPEN1=1 SRE=7`、`COACHZ: first timer interrupt`、PID 1 那行，然后是 `COACHZ init: timer tick 1` 到 `4`。这说明定时器中断能从 GIC 到达 `rtclock_intr`，线程也能按定时被唤醒，`select` 超时后程序继续运行。这是屏幕证据，没有用外部时钟测量 tick 的实际间隔。

`ISENABLER0` 的预期值是 `0x8ffff`，实际读回 `0x87eff`：第 19 位已置上，第 8 和第 15 位没有生效，`IGROUPR0` 也读回 0。原因未验证，可能是非安全访问看不到或被安全固件保留。内核目前只用到 SGI 0 和这个定时器。

同一轮还查清了另一个现象：同一次写入的 U 盘第二次按 Ctrl+U 无法启动。U 盘本身正常，是镜像设置了 `tries=1, successful=0`，固件第一次启动就用完次数并标记坏分区，Debian 下读到的分区属性为 0。屏幕镜像现在设为 `tries=15`，写入后属性是 `0xf1000000000000`，启动一次后读到 `0xe1000000000000`，符合每次启动扣减一次。其他镜像仍是一次。

未解决：随机 zone panic 最近几轮未出现，原因仍未查明。没有 SMP、完整组件 prelink、存储驱动、launchd 或 iPadOS 启动。


## 2026-10-31：prelink 内核在真机上运行 init

运行时与配置见随本节提交，镜像 SHA-256 `5d759e75eb57933c65c36d74751dba2d695d60746bd15b8c92dd86407512c391`。内核是 `platform/coachz-prelink`，合并了 iPadOS 17.7.11 kernelcache 里 OS profile 的组件：共 31 个，其中 `com.apple.iokit.IOHIDEventDriver` 是无代码组件。

第一轮（只有 trust cache，没有 PMGR 补丁）的屏幕照片已经显示 `com.apple.AppleFSCompressionTypeZlib`、APFS、IOSurface、AppleImage4 的启动输出，`BSD root: md0`，以及 `init running as PID 1` 和 tick。init 不再依赖内置 AMFI 接口，而是由真实 AMFI 放行，判断依据是 init 的 cdhash 在自建 trust cache 里，并且这一版内核没有 `coachz_amfi.c`。约 5 个 tick 后出现 `panic: "AMFI: No PMGR?" @ConfigurationSettings.cpp:235`，调用栈经过 `thread_call_invoke`、`thread_call_thread`，即 AMFI 的后台线程等待 `ApplePMGRNub` 超时。

第二轮加入 `coachz-prelink-pmgr.patch`，发布一个没有 provider 的 `ApplePMGRNub`。屏幕文字回绕后可以看到 tick 依次走完 1 到 9，再到 0、1、2，也就是至少 12 个，超过了上一轮 panic 的位置，没有新的 panic。

这只有屏幕证据，没有逐字段核对，也没有验证 tick 间隔或更长时间的稳定性。运行的仍是自写的静态测试程序。`AppleImage4: magazine ... cannot be written`、`serializeVariables failed` 和 `Failed to open swap file` 都出现了，这一轮没有处理，它们符合只读内存盘根和没有 NVRAM 控制器的状态，但没有逐项验证是否无害。ADT 没有 PMGR、存储、显示等硬件节点，所以大部分组件只是加载，没有驱动硬件。

未解决：随机 zone panic 原因仍未查明。没有 SMP、存储驱动、launchd、dyld 或 iPadOS 用户态。


## 2026-10-31：Apple launchd 在恢复内存盘上运行

运行时与配置见随本节提交，镜像 SHA-256 `594123564bff4ad20b17fb8626021e73f8f8aa6a7046ad992720a7af1c3e5142`。根是 IPSW 的 Update 恢复内存盘（178 MiB APFS），信任来源是它自带的 `RestoreTrustCache`。这个内核没有存储、USB 或 SPI 驱动，设备树也没有这些节点。

第一轮：屏幕停在绿块约 2 分钟以上，之后出现 launchd 日志，时间戳约为 `00:02:38`。原因是内存盘在负载里没有 8 字节对齐，探针在缓存关闭时逐字节复制。屏幕显示 `com.apple.xpc.launchd` 启动 `restored_update`（PID 3），它运行了 dyld 和 ObjC 运行时，然后执行 `/sbin/mount_tmpfs /mnt5`（PID 4），之后一直没有新输出。kernelcache 里有 tmpfs，但 OS profile 没有列出它，`mount_tmpfs` 找不到时会 fork 并执行 `/sbin/kextload`。当时停住的确切位置没有查明，这也是这一轮之后才加入线程诊断的原因。

第二轮：改为 16 字节对齐，把 tmpfs 加入 prelink 并在挂载根之前加载。屏幕日志时间提前到约 `00:01:16`，符合复制变快，但没有直接测量。`restored_update` 越过了 `mount_tmpfs`，这是根据后续输出推断的，那一屏 `COACHZ: tmpfs load` 和 `COACHZ exit: pid 4` 已被覆盖，没有直接看到。随后屏幕依次显示：

- `restored_update: disable_watchdog failed`、`COACHZ exit: pid 5 restored_update status 0x100`。它的字符串说明这一步在找 `IOWatchdog` 服务。
- `restored exited normally with status 1 - rebooting`，反复等待 `IOResourceMatch = IONVRAM`，然后 `unable to set restored-exit-status`。
- `Searching for NAND service`，反复 `Waiting for NAND service to publish`。
- `COACHZ alive: 177 s`，同时 `restored_update[3]` 的 `x16` 是 `0x14e`（`__semwait_signal`），即它在循环中睡眠。

这说明 Apple 的 launchd、dyld 和恢复服务可以在这台机器上运行，内核在它们退出后仍然存活。这只有屏幕证据，没有逐字段核对。绿块左边的进度方块用户没有反馈，还没有确认。

这不是主系统：没有主系统卷、共享缓存、界面或显示驱动。`restored_update` 没有走到等待电脑连接那一步，这一轮不打算补 IOWatchdog 和 NVRAM，因为恢复环境不是目标。`kpersona_find_by_type() failed` 的影响没有确认。

未解决：随机 zone panic 原因未查明。根文件系统的既定路线是先做 USB 主机和大容量存储，从 U 盘启动。


## 2026-10-31：探针通过 USB 读到外接盘

运行时与配置见随本节提交，镜像 SHA-256 `982054a6745c32f3926202f764158bf57517a323229ecd7302ee8238b8394b86`。合约见 [USB 只读实验](usb-read.md)。不启动 XNU，只发读命令，读完回到 Linux。

第一轮（U 盘）：MMIO 正常，SMMU 旁路且 `sGFSR=0`，但 `disk=0`，等待 67.8 秒，`USBSTS` 变成 `0x1015`（HSE、HCE）。

第二轮（加了诊断）：`DCBAAP` 读回的地址和内存池不符，初始化刚结束就有 HSE。反汇编确认 `DCBAAP` 被写成了单字节存储，原因是 `strict_align` 和 `__packed` 的组合，不是硬件、SMMU 或内存位置的问题。

第三轮：去掉寄存器结构体的 `__packed`。真机日志严格字段全部通过：`DCBAAP` 正确，`USBSTS` 没有错误位，742 ms 内枚举到硬盘，SuperSpeed，512 字节扇区，读到 `55AA`、`EFI PART`、`CHROMEOS`，`SMMU sGFSR` 全程为 0，随后正常进入 Linux。

这是探针环境，MMU 和缓存都关着，也没有用中断，不代表 XNU 里已经能用 USB。

未解决：第二轮的备用内存地址没有通过 `fdt_range_clear`，备用尝试实际没有运行，这段代码目前没有被验证过。


## 2026-10-31：XNU 内核通过 USB 读到外接盘

运行时与配置见随本节提交，镜像 SHA-256 `aef9eacee6e7ef02f84711bd861c399b68a7a8988d8b6cae0cdde8bf8700052c`。prelink 内核里加入了 `platform/coachz-prelink/drivers/RamBlock`：块设备类沿用 UniXNU RamBlock，后面接探针验证过的同一套 libpayload xHCI 和大容量存储栈，轮询、只发读命令，向系统报告写保护。

第一轮：屏幕显示 `kmem_alloc(..., 16777216, 0x100021): failed with 3 @vm_kern.c:180`。调用栈是 `NativeRamBlock::start → coachz_usb_open → ml_io_map_wcomb → io_map → kmem_alloc`，USB 控制器还没有初始化。ARM 的 `io_map` 从固定 8 MiB 的 `io_submap` 分配（`osfmk/arm/io_map.c` 的 `IO_MAP_SIZE`），16 MiB 的 DMA 池放不下，`KMA_NOFAIL` 让失败直接 panic。

修正：DMA 池缩到 1 MiB。探针真机成功那一轮堆最多用了 176 KiB，内核再加 64 KiB 传输缓冲区。池子的物理地址核对过真机设备树：在普通 RAM 段内，不与任何保留区重叠，也不在 XNU 的物理窗口内。

第二轮：屏幕照片依次显示 `COACHZ USB: mapped 1 1`、`controller 1`、`disk 1 after 787 ms`，`USBSTS 0x18`（没有 HSE、HCE），`fatal 0`，池子用了 240168 字节。随后是 512 字节扇区、`speed 3`，三个扇区 `lba 0`、`lba 1`、`lba 2048` 都是 `ok 1`，分别读到 `55AA`、`EFI PART`、`CHROMEOS`。这是 XNU 第一次在真机上自己枚举 USB 设备并读到外接盘的真实内容。之后 APFS 加载、内存盘作根、launchd 照常运行。

只有屏幕证据。照片里没有出现每分钟一次的 `COACHZ media:` 行，IOKit 块设备、GPT 分区和 IOMedia 是否发布还没有确认。没有写外接盘、固件或内置盘。

未解决：根文件系统仍是内存盘。随机 zone panic 原因未查明。


## 2026-10-31：恢复环境从外接盘挂根

运行时与配置见随本节提交。镜像在内核分区后多了一个 GPT APFS 分区，里面是此前作为内存盘验证过的同一个 IPSW 恢复卷；内存里不再放 `RAMDisk`，只放它的 trust cache，启动参数是 `rd=disk1s1`。外接盘由随本节提交的免密写盘器写入并完整回读，写盘器只写核对过型号和容量的那块外接盘。

第一轮（镜像 SHA-256 `39ece1b8088b91432841c4da628fe2fd17836661e75c6d6f89823967c85c5064`）：屏幕显示 `Got boot device = .../NativeRamBlock/IOBlockStorageDriver/Coachz USB mass storage Media/IOGUIDPartitionScheme/Coachz root@2/AppleAPFSContainerScheme/AppleAPFSMedia/AppleAPFSContainer/ramdisk@1`，`BSD root: disk1s1`，APFS 打印 `mount-complete volume ramdisk`。随后 APFS 判定这是需要根哈希认证的根卷（`could not retrieve system-volume-auth-blob from device tree`），内核 panic：`rootvp not authenticated after mounting`。内存盘根不经过这项检查。

修正：`coachz-prelink-root.patch` 在这个 panic 之前加一道判断，只有 Apple 的认证失败、根卷只读挂载、且它的 IOMedia 沿 provider 链能追到 `NativeRamBlock` 时才放行，并在屏幕打印 `COACHZ root: ... accepted` 或 `refused`。这不是验证：这台机器本来就没有可以延伸的信任链，根卷也不是封存卷。UniXNU 的 owner root 签名清单路线没有接入。

第二轮（镜像 SHA-256 `dcae41b0b501f20be8c40e92032b49db8ee4f2e3d57f81fa1132d7eec666f1fc`）：没有 panic。`restored_update` 从外接盘根运行，行为与内存盘时相同：`disable_watchdog failed`，`exit: pid 5 restored_update status 0x100`，`restored exited normally with status 1 - rebooting`，之后循环等待 `IONVRAM`，并开始反复打印 `Waiting for NAND service to publish`。内核在 140 秒时仍存活，挂根前和心跳时的 IOMedia 列表为：`disk0`（GUID_partition_scheme）、`disk0s1`（ChromeOS 内核分区）、`disk0s2`（APFS 分区）、`disk1`（APFS 容器）、`disk1s1`（APFS 卷）。

只有屏幕证据。照片里没有拍到 `COACHZ root:` 那一行；没有 panic 且根上的程序开始运行，说明放行生效，这是推断。根是只读的，没有写外接盘、固件或内置盘。这仍是恢复环境，不是主系统。

未解决：随机 zone panic 未解释。


## 2026-10-31：iPadOS 主系统卷从外接盘挂根

外接盘按 GPT 依次放内核分区、IPSW `OS` 组件的原始 APFS 容器（UDIF 解出的那个分区）和 `Cryptex1,SystemOS` 的原始 APFS 容器，内容逐字节来自 IPSW，不进仓库。系统卷和两个 cryptex 的三个 trust cache 一起放进内存里的 trust cache 段。整盘镜像由 `tools/linux_log.py --xnu-root-partition` 生成，第一次在 Mac 上整盘写入；之后的轮次用 `--xnu-kernel-only` 只重写内核分区，GPT 只随布局变化，不随内核变化。

第一轮（整盘镜像 SHA-256 `243fd48f336f9438e014681f672ee10ed8f2e0b59900722d5f9b237d379a37a0`，`rd=uuid boot-uuid=`）：两个 APFS 容器和卷都发布了，但内核一直等 `boot-uuid-media`，随后 panic `Failed to mount root device`。这个资源由 macOS 的文件系统驱动发布，这里没有。改用 ADT `/chosen/root-matching`，按 IOMedia 的 `UUID` 属性直接匹配系统卷，`IOFindBSDRoot` 先于 `rd=` 读取它。

第二轮（内核前缀 `b7aa8e02c50c483f55497f0ef56d48f8dba01b6e75ab353edfda869d871d30a8`）：系统卷挂成只读根，原版 `/sbin/launchd` 作为 PID 1 被 exec。dyld 运行了 libignition，找不到 Preboot 卷（role 0x10），随后 `Library not loaded: /usr/lib/libSystem.B.dylib (no dyld cache)`，内核 panic `initproc failed to start`。

第三轮（`b3d76acbc896cb5dab22b57f39856c8f44f2e8fa8c9310aa4b97e278400f2b98`，挂载点为 `/System/Cryptexes/OS`）：`COACHZ SystemOS: mount /dev/disk2s1 at /System/Cryptexes/OS: 2`。系统卷里的 `/System/Cryptexes/OS` 是指向 `../../private/preboot/Cryptexes/OS` 的符号链接，真机上那里挂 Preboot 卷。

第四轮（`f6162513853915a2bd53ddd2baa10f0414cf714c8640467fc757f1880f64a48c`）：按 UniXNU 的非 union 做法，把 SystemOS 只读挂到 `/private/preboot`，设置 dyld 的禁用 ignition 标志，并给 launchd 传 `DYLD_SHARED_CACHE_DIR`。屏幕显示 `mount /dev/disk2s1 at /private/preboot: 0, dyld_flags 0x8`，dyld 找到了 `dyld_shared_cache_arm64`，但内核拒绝映射：`not on process' root volume nor preboot volume`。共享缓存只能来自根卷或 `/private/preboot/Cryptexes` 所在的卷。

第五轮（`ccd81166fb7a993cdfc4a2e5c28b273df236c3c8ac30874018344aedebce48bf`）：`vm_unix.c` 只额外放行内核自己挂上的那个只读 SystemOS 挂载。这一轮在挂根之前就 panic：`pmap_ttne: Attempt to demote L2 block`，调用栈 `bsd_init → bsd_bufferinit → kmem_alloc → pmap_enter`。屏幕控制台在 `arm_vm_init` 里把帧缓冲区以 2 MiB 块映射到低端全局变量和内核之间，但这段虚拟地址没有从 kernel map 预留，kmem 的随机区间可能分到它。此前未解释的随机 zone panic 很可能同源，这一点未证实。

修正：`coachz-screen-console.patch` 记下别名映射的范围，`pmap_virtual_region` 把它报告为第 4 段，`kmem_init` 在放置 kmem 区间之前就占住它。这个补丁两条屏幕路线共用；屏幕路线本身没有重新上机。

第六轮（`791e86405fcc19a5ce8ee55d5c7db2baa900773a6bbecae16bc174eae0d05505`）：没有再出现 `pmap_ttne` panic。SystemOS 挂上，共享缓存映射通过，原版 launchd 真正运行：屏幕显示 `hello, launchd UUID`、`Darwin Bootstrapper Version 7.0.0`、`boot-mode set by sysctl: main`、`entering ondemand mode`，随后执行启动任务。`exclaves-boot` 和 `fsck` 通过，`fsck` 判定不是主 OS 或诊断 OS 而跳过检查。`mount-phase-1` 的 `mount -P 1`（PID 3）打印 `data volume missing, but not required in env: 0`、`DT_get_fstab_entries: failed to get filesystem entry at: /filesystems/fstab`、`failed to get boot manifest hash`，以 66 退出，launchd 因关键启动任务失败而 panic。UniXNU 记录过同样的三行和退出码，缺的是 `/chosen/boot-manifest-hash`。

修正：主系统 ADT 的 `/chosen` 写入一个 48 字节的 `boot-manifest-hash`，是固定标签和两个卷 UUID 的 SHA-384。它只是这组未签名镜像的身份，不是度量，也不验证任何东西。第七轮（`3d0c1243dea59facdbaea8c84faf277e1d8018b879d119ddb9bb1ee5601df3fa`）：mount 阶段通过，launchd 继续执行 `fips`、`keybag`、`usermanagerd`、`dirs_cleaner`、`xpcroleaccountd`、`init_featureflags`、`auearlyboot` 等启动任务。`tzinit` 以 30 退出（`force_system_dataset() failed: Read-only file system`），launchd 写 `/private/var/log` 也一直报 30，随后 panic。缺的是可写的 `/private/var`。

第八轮：Data 种子作为内存盘 md0，内核用 UniXNU 的挂载参数把 `/dev/md0` 挂到 `/private/var`，返回 2。APFS 通过 IOMedia 挂卷，md0 在 IOKit 里没有。

第九轮（`a65caafeec7d9b4f0b8a676370dacb9b9c76f37c44849429d4c339571a8cffab`）：USB 驱动把 md0 那段内存发布成可写的 IOKit 块设备 `CoachzDataBlock`，APFS 在上面发布容器和 `Data` 卷，挂载时 panic：`unencrypted data volume is not allowed`（`apfs_vfsops.c:2276`）。APFS 按卷角色限制：Data 角色的卷不能不加密。

第十轮（`949d85cb313118e25bbe8d2c25119245206bb13f5748ebdcfd6e2ae819de7f29`）：在 `/chosen` 加 `ephemeral-storage = 1`，同样的 panic，这个属性已撤掉。

修正：Data 种子由 `.tools` 里的私有脚本在 Mac 上用 sudo 生成，复制系统卷 `/private/var` 的骨架（不含 `staged_system_apps`）并保留真实属主，卷不设角色。第十一轮镜像 `ebaeb9b7fec898d62faf949258eaf0014a99caedcdb227dfed0d760ec952110a` 已取得屏幕结果：`COACHZ Data: mount ... at /private/var read-write: 0`，无角色的 APFS 种子挂载成功。launchd 越过此前的 tzinit 失败，继续执行多个系统服务；本轮最终停止在 watchdogd。

都只有屏幕证据。根和 SystemOS 都只读，`/private/var` 是可写的内存 APFS 种子，重启丢失；放行未封存根卷和放行 SystemOS 共享缓存都不是验证。


## 2026-10-31：可写内存 Data 挂载成功，启动停止于 watchdogd

运行时 `ac5e2c0`，第十一轮镜像同前。屏幕直接显示 SystemOS 挂载成功和 `/private/var read-write: 0`，并显示 launchd 执行 `contextstored`、`WirelessRadioManagerd`、`healthd`、`usernotificationsd`、`countryd`、`mobileassetd`、`usermanagerd`、`watchdogd` 等进程。这里只确认进程执行和挂载结果，不据此认定所有服务初始化成功；例如屏幕另有 usermanagerd 的异常退出。

最终关键进程退出为 `watchdogd`，`exit reason namespace 2 subcode 0x5`、description none，随后触发关键进程退出 panic。按固定 XNU 的 `bsd/sys/reason.h` 与信号定义，这是 `OS_REASON_SIGNAL / SIGTRAP`。core dump 还因目标文件无法创建而失败；这个后续错误不是已定位的 SIGTRAP 根因。没有真实用户态故障 PC 或调用栈，不能直接认定是缺少 IOWatchdog。

同版原始 watchdogd 与 launchd 经摘要核对，与锁定 UniXNU 已有分支分析输入一致。复用其 `check-watchdogd-branches.py` 的有界离线执行，四种分支通过：原生环境没有 IOWatchdog 服务时主动 BRK；虚拟环境缺服务时正常退出；打开服务失败时另一处 BRK；正常连接时返回成功。外部 API 是桩，这验证原始二进制控制流，不是这次真机故障位置的证明。

下一轮只增加关键进程 panic 前当前退出线程的保存 EL0 PC、LR、SP、FP、可执行文件基址与少量寄存器输出，便于按 ASLR 还原位置。保持原版服务、信号处理和 panic 策略，不添加虚假的 watchdog 硬件服务，也不全局禁用关键进程 panic。补丁为 `coachz-prelink-critical-state.patch`；当前无该诊断的真机结果。


## 2026-10-31：watchdogd 用户态现场诊断介质准备

本轮仅增加关键进程 panic 前的当前退出线程现场输出，原有 Data 种子、信号处理与关键进程 panic 策略不变。源码内核与组件 prelink 构建成功，最终内核中确认新增符号和输出字符串；已有 26 项显示／用户态路线回归通过，原版 watchdogd 的四种离线连接分支通过，外部 API 为桩。

内核前缀镜像 SHA-256 `bb7a9f3ad8d1192b580329010986427c30f2c1b75a218270c8199ccfe77fc86c`，签名容器 SHA-256 `9502a0c19bf9e56d33faa256cf6ca5672bb666d35846ab37cfc23e4afff9c549`。10727 条指令／20 段标量审计通过；重新核对清单源码和全部产物摘要、实际嵌入的本次 prelink 内核，GPT 布局与此前镜像一致。

通过既有免密写盘器核实外置介质与运行系统排除，完成 225 MiB 内核前缀写入和全量回读，写盘器返回零。没有改动系统卷或 SystemOS 分区，没有写固件或内置盘，没有自动重启。尚待手动启动取得 `COACHZ critical` 的 PC／LR／可执行文件基址；不能把介质准备或离线 BRK 分支当作真机根因已定位。


## 2026-10-31：watchdogd 真机故障偏移确认与临时策略

运行时 `0bab269`，使用前述同一诊断镜像。屏幕取得 `COACHZ critical watchdogd` 的 PC、LR、SP、FP、可执行文件头与寄存器；按头地址消除 ASLR 后，PC 相对主程序头的偏移为 `0xab0c`，与同版原始 watchdogd 的缺少 IOWatchdog 分支内 BRK 完全相符。信号仍是 SIGTRAP，服务查找分支对应的寄存器为零；这次把此前的离线分支判断与真机停止位置接上。原始进程地址和照片保持私有。

下一轮使用 launchd 已有的根域禁用策略，只临时禁用 `com.apple.watchdogd`，让启动继续暴露后续依赖。`coachz-disable-watchdogd=1` 仅由带内存 Data 种子的主系统打包路径加入；成功挂载内存 `/private/var` 后、launchd 执行前创建 root 所有且不可由组／其他人写入的 `disabled.plist` 与 `disabled.migrated`，全部随内存 Data 在重启时丢失。拒绝覆盖已有文件，失败明确报告。没有修改原版 launchd 或 watchdogd、没有伪装 VM，也没有禁用全局关键进程 panic；这不是看门狗硬件支持。

同版原始 launchd 的有界离线执行使用本次实际 XML：root 所有策略禁用 watchdogd、非 root 策略拒绝、其他 job 保持启用，三种情况通过；文件系统和 libxpc API 为桩，不能提前认定真机会采用该策略。保留已验收的 critical-state 输出，下一轮实际启动结果仍待取得。


## 2026-10-31：临时 watchdogd 启动策略介质准备

补丁为 `coachz-prelink-watchdog-policy.patch`，由带内存 Data 的主系统负载显式启用，保留现有 critical-state 诊断。源码内核与 prelink 构建成功；27 项显示／用户态路线回归通过，新增一条实际负载打包回归确认无内存 Data 的路线不会自动启用策略。原版 launchd 的三种离线策略情况和实际 XML 校验通过；真实 VFS 创建和真机策略采用仍待确认。

内核前缀镜像 SHA-256 `20234ad1eec4267c96c6cd7cb6e2053d65a7cb9dd06c8a1206f1b12e331cd1c8`，签名容器 SHA-256 `7d40bff4cf27aac2feb9c284b292c6f33c5e382a12ca2df03dfc32f04f5915f1`。10727 条指令／20 段标量审计通过；独立重算清单全部源码和产物摘要，核对当前 prelink 内核确实嵌入、显式启动参数存在且没有全局 panic 禁用参数、Data 种子字节未变、GPT 与前一轮一致，前缀结束位置恰在系统分区之前。

目标端传输摘要一致，既有免密写盘器核实外置介质及运行系统排除，完成全部 225 MiB 前缀写入与回读，返回零。未改写系统卷、SystemOS、固件或内置盘，没有自动重启。下一次手动启动观察策略结果行及是否越过 watchdogd；如出现新的关键服务退出，保留其现场，不能预先宣称主系统或界面启动成功。


## 2026-10-31：越过 watchdogd，usermanagerd 重启循环

运行时 `f98d3aa`，使用前述临时 watchdogd 策略镜像。屏幕照片显示启动继续推进，没有重现 watchdogd 关键进程退出 panic，并仍有周期性的进程／线程采样。`usermanagerd` 反复以 `status 0x6` 退出，按固定 XNU 的 wait 状态解码为 SIGABRT；`fairplayd` 反复以零状态退出，launchd 经 xpcproxy 创建新实例。递增的 PID 反映这些创建活动，不是存活进程数，也不证明后续服务初始化或界面成功。

只读对比发现当前内存 Data 种子缺少锁定 UniXNU 离线 Data 准备脚本创建的部分空目录，包括 dslocal、PersonaVolumes 和 keybags 的目录骨架。这是待核实的依赖差异，尚不能据此认定 SIGABRT 的原因。原版 usermanagerd 还包含会话创建断言；本轮不跳过该服务、不改变密钥包启动参数，也不先补目录来掩盖第一处失败。

下一轮增加 `coachz-prelink-usermanager-diagnostic.patch`：只为 usermanagerd 保存有界的首次 stdout/stderr 写入缓冲，在首次 SIGABRT 的退出路径记录原有 exit reason、当前线程保存的 EL0 PC／LR、可执行文件头、共享缓存 slide 与最多八帧用户栈链。所有用户内存读取使用 copyin，栈链限制在初始 SP 上方 64 KiB 内；每分钟采样末尾重显，避免后续 respawn 刷走现场。它保留信号、原版二进制与退出策略。空白 stdio 或不完整栈链仍需结合 PC 和原版二进制分析，不能当作没有错误。


## 2026-10-31：usermanagerd 首次 abort 诊断介质准备

源码内核与 prelink 构建成功；已完成的 27 项显示／用户态路线回归全部通过。诊断文件使用 libkern/copyio.h 引入用户内存读取接口，避免在 osfmk 文件中引入完整 BSD systm.h 的类型冲突；使用实际编译参数单独编译通过后，再完成正式构建。最终内核中确认首次 stdio、abort reason、用户态现场与栈帧输出字符串存在，真机执行和实际捕获效果仍待确认。

内核前缀镜像 SHA-256 `6a31538bd8fe563d04e1c08017be542f980f4b8b23390e570d1f875bc3388f65`，签名容器 SHA-256 `4b051503f22467e8298c983afcd67b0b058fe5a8d886d8341347c6e71868d67c`。10727 条指令／20 段标量审计通过。写盘前独立核对清单全部源码及产物摘要、实际内嵌的本次 prelink 内核与诊断字符串；原有 watchdogd 策略参数仍在，没有全局 panic 禁用参数；GPT、Data 种子与上一轮逐字节相同，前缀结束于系统分区之前。

既有免密写盘器核对目标与运行系统排除，完成全部 225 MiB 前缀写入和回读，返回零；系统卷与 SystemOS 未改写，没有自动重启。下一次手动启动后等到分钟采样末尾，取得 `COACHZ UM original stdout/stderr`、`COACHZ UM abort`、`COACHZ UM state` 和可用的 `COACHZ UM frame` 行，再按可执行文件头或共享缓存 slide 还原故障位置。本轮只增加可观察性，尚未修复 usermanagerd。


## 2026-10-31：首版 usermanagerd 诊断未取得可见现场

运行时 `da224a1`，使用前述首次 abort 诊断镜像。照片仍显示 usermanagerd 以 `0x6` 反复退出、fairplayd 零状态退出及周期线程采样，没有取得预期的 `COACHZ UM` 现场。不能把这个结果记为诊断验收通过，也不能由照片断定钩子一定没有执行。

核对最终内核机器码确认 proc_prepareexit 内确实调用新增 helper，周期采样也确实调用 report；原版钩子同时要求目标是 current_proc 且 wait 状态为 SIGABRT，report 在没有捕获时静默返回。此外，屏幕控制台换行在行数边界循环回顶部覆盖，并非保留历史的滚动终端。这些实现使现有照片无法区分未命中、输出被覆盖与其他退出上下文，尚未确定 usermanagerd 的根因。

本地修订的诊断 v2 去除状态和 current_proc 的过滤，只按目标名称观察首次退出，明确输出原始 wait 状态、处理者 PID、自身／其他上下文和 exit reason。只有自身退出才 copyin 当前用户栈；其他上下文仅保存目标任务最多三个线程的 PC／LR／x16 快照，不将其冒充故障线程或调用栈。完成记录后用 release/acquire 发布。采样每十秒显示 v2 标记或完整现场，捕获后暂停全量线程／介质日志，降低覆盖速度。原版进程、退出策略和 Data 种子不变。源码内核与 prelink 已构建，27 项既有基础回归通过；最终机器码中的退出与报告调用已核对。镜像 SHA-256 为 `b7306195702fdeaff77ecd67482548f2448f51a26522e2a23d7f0724af9ec48f`，实际内嵌内核、v2 字符串、清单、相同 GPT 与 Data 字节已核对；既有免密写盘器完成 225 MiB 前缀写入与全量回读并返回零。没有自动重启，真机诊断仍未验收，调试修改保持未提交。


## 2026-10-31：v2 捕获正常初始化任务，确认 keybags 目录缺失

前述本地 v2 镜像的屏幕照片取得了 `COACHZ UM v2` 记录、保存的 EL0 状态和三帧用户栈链，可见性修订实际工作。这次记录的 wait 状态为零、自身上下文、reason 为零，属于正常退出的早期 usermanagerd 初始化任务，不是之后 daemon 的 SIGABRT；首个退出锁存使后续异常未被保留。

原版输出显示 Default persona 创建成功，随后原子写入 `/private/var/keybags/persona.kb` 的临时文件 `persona.kb.writing` 时，open_dprotected_np 返回错误 2／No such file or directory。结合之前对 Data 种子的只读目录核对，keybags 父目录确实缺失；这是已经取得的具体文件系统失败，尚不能单凭它断定后续 SIGABRT 的完整根因。

本地修订 `coachz-prelink-keybags-directory.patch` 只在已成功挂载的内存 Data 上创建空的 keybags 与 backup 目录，走原有 mkdir 系统调用和 VFS，使用根所有的 0700 目录并复核类型／所有者，原版 usermanagerd 自己生成 persona.kb；没有写入人工密钥、用户记录或系统卷。诊断 v3 单独记录正常任务次数与末尾输出，正常退出后继续等待首次非零退出，保留后者的 reason、上下文和可用的用户态现场。本地源码内核与 prelink 构建通过，27 项既有基础回归通过。最终机器码确认真实 mkdir／VFS 目录校验和正常／异常分流，镜像 SHA-256 为 `0141f559173c7640ae03f9cbbb568b8ecc0067c43a9765742803a2fa1cc00158`；实际内嵌内核、清单、相同 GPT 与 Data 种子逐字节核对后，既有免密写盘器完成 225 MiB 前缀写入及全量回读，返回零。没有自动重启；这些是测试准备，修订保持未提交，目录问题与后续 daemon 行为仍待真机确认。


## 2026-10-31：补齐 keybags 目录后推进到 SpringBoard 进程

使用上一节同一镜像，屏幕显示 v3 正常初始化退出计数为一，原版 UMD_INIT 在此前的 persona.kb 原子写入报错点之后继续输出已加载的 System／Default persona、选择用户 501、加载前台用户及普通模式会话设置；未再显示原先的目录不存在错误。在两个不同时间的照片中，v3 均仍等待异常退出，尚未捕获到 usermanagerd 的非零退出。这是本次观察区间内的结果，不代表所有用户管理功能或长期稳定性已验收。

同次启动的屏幕出现 SpringBoard 的退出记录，wait 状态为 `0x6`／SIGABRT，证明启动已推进到该进程的退出路径；尚没有可见界面，也没有其真实故障 PC／调用栈。locationd、sharingd、chronod 等也出现 SIGABRT，liveactivitiesd 则为 SIGTRAP；不据相同信号断定相同根因。

`Too many corpses being created` 是 proc_prepareexit 在原有崩溃处理阶段调用 task_mark_corpse 失败后的诊断，不是本次进程为何 abort 的解释。下一步保留已通过的目录准备与 usermanagerd 诊断，单独记录 SpringBoard 的首个非零退出 reason、上下文、可执行文件头、共享缓存 slide 和可用用户栈，先定位实际依赖失败。


## 2026-10-31：SpringBoard 首次异常现场诊断（本地修订）

在已验收的 keybags 目录准备之上，新增独立 `coachz-prelink-springboard-diagnostic.patch`。它仅观察 SpringBoard：有界保存 stdout/stderr，在首次非零退出时记录原始 wait 状态、reason namespace／code／description、处理上下文、Mach header 与共享缓存 slide。自身退出时保存 PC／LR／x16，并通过 copyin 读取最多十二帧、限初始 SP 上方 64 KiB 的用户帧链；其他上下文仅记录目标线程的标注快照。

记录完成后每十秒重显末尾输出及现场，并优先于全量进程／介质采样，避免循环屏幕覆盖。user-manager 诊断和通过验收的目录准备保持原样；原版进程、trust cache、信号与关键进程退出策略不变。同版原始共享缓存与本地符号 UUID 已核对匹配，为实际硬件帧定位准备输入。锁定 UniXNU 的 FrontBoard 显示断言分析仅作参考，不能替代本机故障位置。源码内核与 prelink 构建通过，27 项既有基础回归通过；最终机器码已核对退出、普通 write 路径与周期报告三个实际调用。镜像 SHA-256 为 `8b998ea52af6eb14ba1f3c30a2257841d57342bd3761246388ef8903bfa30dad`，实际内嵌内核及 SB 诊断字符串、已验收目录准备字符串、相同 GPT 与 Data 种子核对后，既有免密写盘器完成 225 MiB 前缀写入与全量回读并返回零，没有自动重启。普通 write 路径只保留片段，空白 stdio 不代表没有日志；可用帧链也不保证完整。新诊断仍未实机验收，保持未提交。

## SpringBoard 首次异常：Objective-C trampoline 路径缺失

独立的 SpringBoard 诊断已在真机照片中输出首个异常退出：`status 0x6`，
`OS_REASON_OBJC`（namespace 8）、code 1。原始说明为无法 `dlopen`
`/usr/lib/libobjc-trampolines.dylib`：普通路径不存在，亦不在 dyld cache 中。
已取得可执行文件基址、cache slide、PC/LR 与 12 层用户栈；原始现场保留在私有记录中。
这确认了装载命名空间故障，不表示已进入正常图形界面，也不能由此判断 GPU 是否可用。

只读检查同一 IPSW 的原始 OS 与 SystemOS：该文件不在 OS，却在 SystemOS 的 `usr/lib`
中。之前仅挂载 SystemOS 到 `/private/preboot` 并设置共享缓存搜索目录，未合成普通
`/usr/lib` 路径。因此当前实验改用 XNU 原生只读 union root，沿用已选架构的 lower-directory
及 getattrlistbulk 补丁，检查真实挂载关系及规范路径的原始 dylib，移除该模式下的缓存目录
环境覆盖。保持旧恢复环境与显式内存 Data 路线。

### 下一轮介质配置：尚未实机验收

64 MiB 内存 Data 已不适合当前服务。下一轮选择独立 8 GiB、无角色 APFS Data，作为
GPT 第四分区；按卷 UUID 挂到 `/private/var`。种子从原有普通镜像扩容，保留目录及真实
所有者。OS 与 SystemOS 原始容器保持只读；只有经主备 GPT 校验的 Data 扇区窗口允许
USB 写入，所有读写及真实 SCSI cache flush 共用一个锁。原厂 vboot 会在尝试启动时把
备份 GPT 更新到物理盘尾，而主表 alternate LBA 可能仍指向镜像末尾；校验按该实现处理。

分区布局改变后须在 Mac 本地写完整镜像并回读。后续内核前缀只能用于完全相同的 GPT
布局，不能再使用先前的三分区脚本。具体设计见 [外接 Data](plans/usb-data.md)。
构建、分区边界回归和介质回读均不代替这条新路线的真机验收；重启后数据保留也需单独验证。

下一轮普通镜像已完成主机校验：实际嵌入当前 prelinked 内核，启动参数选择 native union
和外接 Data UUID，不包含 `RAMDisk`／`PackedRAMDisk`，未添加全局 panic 或代码签名禁用。
主备 GPT CRC 和容器逐字节校验通过，第四分区与 APFS superblock 均为 8 GiB。
边界测试覆盖物理盘尾 vboot 修复、损坏主备表、读取失败、分区重叠、错误容量与跨界写入；
原有恢复路线回归、五项镜像检查及四种基线探针构建通过。全套 162 项主机检查中 ABI 编译
项被沙箱工具权限阻断，其所属 12 项模块在正常主机权限下重跑通过。以上仍非真机启动验收。

## 2026-10-31：原生 union 与外接 Data 启动路径越过缺库，进入主显示器断言

同一四分区整盘镜像的本地写入与完整回读由操作者完成，摘要匹配，随后手动启动。
照片中的 SpringBoard 首次异常已由此前 namespace 8/code 1 的缺库 SIGABRT 变为
`status 0x5`、`OS_REASON_SIGNAL`（namespace 2）、code 5，且为自身退出上下文。
照片包含本轮共享缓存 slide 与可读帧链。

使用本次 IPSW 原始缓存和 UUID 匹配的 local-symbol 文件独立符号化：PC 对应
`-[FBSDisplayMonitor _initWithBookendObserver:transformer:] + 1148`，随后帧链经过
FrontBoard 的 `+[FBDisplayManager sharedInstance]` block、libdispatch once 和
`-[FBSystemShell _initWithOptions:]` block。原始缓存该 PC 的指令为 BRK；解析此
故障块实际引用的 CFString 得到 `failed to find the main CADisplay`。此字符串是原始
二进制的静态断言引用，并非照片捕获的运行时输出；原始地址和图像留在私有记录中。
`Too many corpses being created` 仍只是后续崩溃处理失败，不能取代这个故障位置。

本镜像在 launchd 前强制验证 union 根关系及规范路径的 trampoline 文件，并在外接
Data 的 GPT、cache sync、卷选择、挂载或目录／策略准备失败时停止。因此 SpringBoard
到达新位置说明这些启动前检查已经通过，且已越过此前原版 Objective-C 缺库退出；这是
根据确切镜像与强制路径作出的判断，照片没有保留所有早期成功行。恢复到 Debian 后，
只读分区检查确认第四分区为 8 GiB APFS。没有再次出现之前带 Data 角色的未加密卷拒绝。
重启后的文件内容持久性尚未单独读回，不将分区存在等同于完整的持久性验收。

下一步是向原版显示框架发布实际固件 framebuffer。现有内核文本控制台不等于存在
IOMobileFramebuffer/CADisplay 服务；此轮没有可见用户界面，也没有 GPU 渲染或触摸支持
的验收。保持原版系统文件、崩溃策略、四分区布局及外接 Data，不再重写整盘来做纯内核迭代。


## 2026-10-31：原版 Legacy framebuffer 后端，待真机启动

根据上述主 CADisplay 断言，下一轮内核加入显式 `--display` 路线：在原版 launchd
之前发布实际固件 framebuffer，调用原版 IOMobileFramebufferLegacy 生命周期，保留
真实 IOSurface 接口。CPU 后端只接受其已限定的 BGRA 帧；刷新通知暂由 60 Hz 软件
计时器提供，没有实现 GPU 或硬件 vblank。启动步骤、客户端、映射和最初帧有屏幕日志，
未知抽象后端调用保留带 slot/caller 的停止诊断，详见 [显示实验](native-display.md)。

完整编译及 prelink 已通过，原版 IOSurface/MobileGraphics 布局和 MobileGraphics
完整可执行段哈希与已验收参考一致；358 项 vtable、原版方法入口及自有延后构造器
执行指令不变检查通过。四种基线探针构建通过。164 项主机回归中 163 项首次通过，
一项 boot_args ABI 编译在沙箱下失败，在正常主机权限下单独重跑通过；共享 CPU
像素复制和边界测试另行编译运行通过。这些结果不代表真机显示服务或 UI 验收。

本轮只打包 225 MiB 内核前缀，主分区表与上一轮整盘镜像逐字节相同，写入边界恰好
停在 OS 分区之前；不重写 OS、SystemOS 或 8 GiB Data。镜像参数选择同一 Data 卷，
不带 RAM Data，包装器标量指令审计通过。既有目标端写盘器已更新此前核验的外接测试盘，
完整 225 MiB 回读摘要匹配，未自动重启。测试仍需操作者手动启动并拍照确认。


## 2026-10-31：持久化 Data 的第二次启动在策略文件重复创建处停止

显示实验首轮照片确认 SystemOS union 与外接 Data 的挂载返回 0，keybags 目录准备也
返回 0；随后 watchdogd 禁用策略返回 17 并触发显式 panic。该错误来自 launchd 前的
文件准备步骤，尚未运行新显示后端，不能据此判断显示驱动成败。

原函数始终以 `O_CREAT | O_EXCL` 创建 disabled.plist 和迁移标记；原先每次重建的
内存 Data 不会保留它们，持久化 Data 在后续启动已有文件时则返回 EEXIST（17）。
主机对原始函数的文件操作重放已复现第二次创建返回 17。下一轮只修正此生命周期问题：
已存在文件以不跟随链接的只读方式打开，验证普通文件、root 所有、非组／其他可写、
长度和预期字节；匹配则复用，缺失才创建。不同内容明确报错，不覆盖已有用户数据。

相同函数的主机重放确认策略和空迁移标记都可连续准备两次，原文件 inode 和内容不变，
不同内容或不安全权限仍被拒绝。这是主机验证；真机尚待下一轮验证。保持显示后端和
四分区布局，只更新内核前缀；此轮没有达到显示服务或 UI 的实机提交条件。

修订内核完整编译及 prelink 检查通过，原版显示组件布局／代码哈希保持一致；33 项相关
回归通过。新 225 MiB 前缀已核对当前内核、重复启动修订日志、原有显示参数和与已验收
整盘镜像相同的 GPT；既有目标端写盘器完成更新，全部前缀回读 SHA-256 匹配，未自动
重启，未重写任何 APFS 容器。没有新的实机通过记录。


## 2026-10-31：显示后端越过主 CADisplay 断言，停在显示模式校验

k20 照片显示 SpringBoard 首个异常退出仍为 `status 0x5`／SIGTRAP，但 PC 已不在
`failed to find the main CADisplay`。用同版缓存和 UUID 匹配的本地符号符号化，调用链为
`FBSDisplayMonitor` → `FBSDisplaySource` → `FBSDisplayConfiguration` →
`-[FBSDisplayMode _initWithCADisplayMode:scale:rotation:]` →
`_initWithWidth:...:validityCheck:` 中的 BRK，断言字符串为
`FBSDisplayMode cannot be configured as requested : %@`。主显示器传入的 scale 为 0，
校验改用模式自带的 preferredScale；该 PC 与 UniXNU 早先记录的缺 scale 失败相同。

k21 在 SpringBoard 诊断中加入断言现场 x19／x24／x25／x26 与 x19 所指模式对象的
80 字节。k21 上机前，Data 卷曾在 Mac 上被自动以读写方式挂载；随后的启动在 launchd 前
校验已有 `disabled.plist` 时返回 13（EACCES）并 panic。k22 在已有策略文件不一致时
打印属主、权限、长度与开头内容，原样另存为 `<path>.coachz-prev`，再写回预期字节并
恢复 root／0644；主机重放测试覆盖另存、写回、权限恢复和一致时不重写。k22 未再出现
该 panic，k23 照片中两个策略文件校验均返回 0。

k22 照片取得模式对象：2160×1440、scaleOverride 0、**preferredScale 0.0**、刷新率
60.0、gamut 1、hdr／rotation／virtual 为 0，确认失败项是 preferredScale。只读挂载
Data 复制出的 MobileGestalt 缓存中，`main-screen-scale`、`main-screen-pitch`、
`main-screen-class` 都属于存放在 CacheExtra 的键，却都不存在；该缓存由没有显示服务的
早先启动生成。libMobileGestalt 的屏幕参数路径读取 `/chosen` 的 `display-scale`，
读不到时默认 0。

k23 的 `coachz-prelink-mgcache-reset.patch` 仅在显示路线（`coachz-reset-mgcache=1`）
于 launchd 前删除这一个缓存文件。照片显示删除返回 0，显示后端报告
`original Legacy active, mode 2160x1440 scale 2`，原版 launchd 正常启动；随后屏幕出现
`COACHZ display: CPU transfer 1 status 0x0 bytes 12441600`（正好一帧 2160×1440×4）、
`frame 2 kind 0 nonzero 41605`，中央显示原版白色苹果标志。这是原版 iPadOS 图形首次经
原版 IOMobileFramebuffer 和 CPU 后端显示在本机屏幕上；不代表 GPU、Metal、触摸或可用
界面。同屏还有 `IOMFB: default_fb_surface: No Default FB Surface found` 提示。

同次启动中 SpringBoard 的首个异常退出变为 `status 0x9`（SIGKILL，自身上下文）。
符号化显示主线程在 LaunchServices 初始化（`_LSContextInitCommon` →
`_LSCopyServerStore`）中经 NSXPCConnection 同步等待 lsd 回复。谁发出 SIGKILL、lsd
是否运行或卡住尚未确认。

同次启动稍后的照片中，屏幕变为原版白色界面，下方显示 `Press home to upgrade`，
`CPU transfer 5 status 0x0 bytes 12441600`。屏幕文字控制台叠在帧缓冲上方，诊断仍保留
首个 SIGKILL 现场；这表明后续重启的 SpringBoard 已绘制原版系统界面。这是等待用户按
Home 继续升级／迁移的画面，与从未完成设置或迁移的 Data 卷相符；尚无触摸或按键输入，
不能继续操作，也不代表可用桌面。


## 2026-10-31：launchd 重写策略文件；稳定启动到升级界面

k24 照片显示，已有 `disabled.plist` 与预期不一致：root 所有、0644、356 字节，开头为标准
XML plist 与 DOCTYPE。这是 launchd 在上一轮运行中按自身格式重写的结果。替换步骤报告
成功，但随后复核仍返回 13：内核 `vnode_open` 不替调用者执行 `O_TRUNC`，短内容只覆盖了
文件开头，长度仍为 356。主机测试的桩函数把 `O_TRUNC` 交给了 POSIX `open`，因此没有发现。

修订后，替换写入时显式设置文件长度；已有文件若仍为 root 所有、不可被组或其他人写的普通
文件，且 `<key>com.apple.watchdogd</key>` 之后紧跟 `<true/>`，即按 launchd 格式接受，
不再改写。测试桩改为与内核一致不截断，新测试对旧补丁失败、对新补丁通过。

同一 k25 镜像还加入 `coachz-prelink-launch-diagnostic.patch`：记录经 `kill()` 发出的前
6 次 SIGKILL（发送者、目标、开机秒数）以及 lsd 与 SpringBoard 的前 10 次启动和退出，
随每 10 秒的报告打印。k25 真机启动没有 panic；SpringBoard（pid 27）于 110 秒启动，lsd 于
123 秒按需启动。291 秒的报告记录 1 次 SIGKILL：`backboardd(57) -> SpringBoard(27)`，
发生在 201 秒，同一秒新的 SpringBoard（pid 172）启动。被杀现场去掉 slide 后的 PC 与上一轮
相同，仍是 LaunchServices 初始化中经 XPC 同步等待 lsd 回复。操作者确认屏幕已变为白底的
`Press home to upgrade` 界面，随后被滚动的文字控制台覆盖。

因此首个 SpringBoard 在约 91 秒未完成启动时被 backboardd 结束，第二个 SpringBoard 绘制界面；
lsd 为何在启动后迟迟不回复（例如首次建库耗时或其依赖未就绪）尚未确认。


## 2026-10-31：帧率与 lsd 慢的共同原因是单颗小核

k26 在显示后端启动时读取 CPU 标识并粗测主频，实现了原先为空的
`native_legacy_copy_timing_observed`，并在启动诊断中于 SpringBoard 启动后约 45 秒、80 秒
各记录一次 lsd 线程快照（状态、阻塞类型、PC/LR/x16，主线程用户栈）。

照片显示 `midr 0x51df805e`（高通 Kryo Silver，基于 Cortex-A55）、`mpidr 0x81000000`
（CPU0），计数器 19.2 MHz，2000 万次计数循环 13195 µs，约每微秒 1515 次。按每次一周期估算
约 1.5 GHz，与 SC7180 小核 1516 MHz 档位相符；这是粗测，不是调频寄存器读数。系统只运行在
这一颗核上。

整帧 CPU 拷贝 220～243 ms（12441600 字节，约 52 MB/s），相邻两次拷贝间隔 5075～6233 ms。
拷贝占帧间隔不到 5%，主要时间花在渲染服务产出帧之前；60 Hz 软件刷新通知不代表实际帧率。
帧缓冲以不可缓存方式映射，写入吞吐仍有改进空间。

SpringBoard 启动后 49 秒的 lsd 快照：5 个线程；主线程处于等待，栈为
`_runServerMainRunLoop` → `CFRunLoopServiceMachPort`，即主循环在等待请求；两个工作队列线程
处于可运行状态。lsd 没有卡在主线程上，结合帧间隔，SpringBoard 等待 lsd 超时与帧率低都与
全部负载挤在一颗 A55 上相符；其余核心（含两颗 A76）尚未启动。


## 2026-10-31：四颗 A55 经 PSCI 启动并参与调度

`tools/xnu_screen.py --smp 4`（只用于 prelink 主系统）在 ADT 中补出 `cpu1`～`cpu3`
（`reg` 0x100/0x200/0x300，state `waiting`）与 `/psci`（`arm,psci-1.0`，method `smc`），
把命令行的 `cpus=1` 改为 `cpus=4`，并加入 `sheng-smp=1 armcpu-self-ipi=1
armcpu-el2-entry=1`。BCM2837 板级配置限定 4 颗 CPU、1 个簇，因此本步只启动 CPU0～3，全为
A55。`coachz-prelink-smp.patch` 让复位向量按 MPIDR Aff0 与 Aff1 查找本核数据：SC7180 设置
MPIDR.MT，核号位于 Aff1，原代码在无 `HAS_CLUSTER` 时只比较 Aff0。TF-A 的 CPU_ON 在
实现 EL2 时从 EL2 启动新核，ArmCpu 私有补丁加入位置无关的 EL2 入口：重复 `boot/entry.S`
的 EL2 设置，打开 `ICC_SRE_EL2`，写入真实 MIDR/MPIDR，再 eret 到以 PSCI context id 传入的
复位向量物理地址。

k27、k28 实际仍带 `cpus=1`：XNU 只解析启动核，ADT 中的其余 CPU 没有逻辑编号，ArmCpu
跳过它们，k28 报告显示三颗从核均停在步骤 0。k27 观察到的“变快”因此与多核无关，原因未确定。

k29 照片中的周期报告：`3 secondary running`，CPU1～3 均为步骤 7、PSCI 返回 0、无失败码；
开机 31 秒时四颗核的定时器中断计数分别约为 23171、22914、22821、22799。显示后端启动时的
测速在 MPIDR 0x81000300（CPU3）上运行，报告测速在 CPU2 上运行，确认调度已使用从核。
四颗核测速均约每微秒 757 次，与 k26 单核时的约 1515 次相比约为一半，与小核 768 MHz 档位
相符；这是计数循环估算，不是调频寄存器读数。四颗核的 SGI 计数均为 0：自 IPI 与跨核 IPI
都未被接收，跨核唤醒目前只能依靠各核自身的定时器。尚未验证 A76 大核、调频、IPI 与长期
稳定性。

### 八核与 IPI（k30～k34）

k30 让 ADT 列出八个核后，CPU6、7 的 A76 上线，随后 cpu 2 在 `thread_select` 的 ticket lock 上
panic：BCM2837 没有 `__AMP__`，调度器是单簇 Clutch，第二个 pset 打开了偷取，而
`sched_clutch_steal_thread` 返回时仍持有 pset 锁。现在所有核共用 pset 0，E／P 簇仍保留在拓扑里。
代价是调度器不区分大小核。

k31 带上游诊断 `armcpu_sgi_selfprobe`，每次都在 launchd 的 `check_np` 之后整机复位，Coreboot
日志有 `PM: WRM_reset by PS_HOLD`（正常重启也会这样）。k33 只把它换成只碰 Group 1、只发
SGI 0～7 的 `armcpu_group1_sgi_probe`，同一位置不再复位。诱因应是这个函数，具体机制未确认；
反汇编显示它实际会写 `ICC_SGI1R_EL1`、不屏蔽 IRQ，并从 EL1 读 `ICC_HPPIR0_EL1`。

k33 的探测仍是 `grp1 0x0`。host 上的反汇编给出了原因：Mach-O 的 AArch64 汇编器把 `;` 当注释，
`"dsb ishst; msr icc_sgi1r_el1, %0; isb"` 只生成一条 `dsb`，所以 `signalCPU` 从未发出 SGI。
内联汇编改用 `\n` 分隔后，k34 的 `signalCPU` 等四个函数都有这条写入。

k34 的照片（开机 31 秒和 41 秒两次周期报告）：

- `7 secondary running, 8 CPUs in 2 clusters`，八个核都是步骤 7、PSCI 返回 0、无失败码。
- CPU0～5 显示 `p805 E`，CPU6、7 显示 `p804 P`。
- 探测在 CPU0 上得到 `grp1 0xff`，SGI 0～7 都能到达。
- 41 秒时各核 SGI 计数依次为 234、186、118、23、5、5、9、1；CPU0～2 在这 10 秒内继续增长。
- CPU0～2 在这 10 秒内定时器中断约增加 40320、52262、7316 次；CPU3～7 几乎不变，CPU6、7
  只有 2 和 0 次。

因此 IPI 已在真机上工作，但大核基本没有分到负载。计数循环在 CPU0 上约为每微秒 757 次，同样
只是估算，不是调频寄存器读数。机主观察到 SpringBoard 的刷新仍然极慢、帧间隔很长，写合并映射
的效果还没有测到。


### 全面屏身份、固定最高频与大核优先（k35）

k35 在 k34 上改了四处：

- ADT 身份改成没有 Home 键的 11 寸 iPad Pro（`iPad8,1`、`J317`、`home-button-type=2`），根
  `compatible` 仍是 `J120AP`，系统仍是这份 A10X 的 IPSW。
- `coachz-max-freq=1` 让 ArmCpu 把 A55、A76 和 L3 三个 cpufreq-hw 域固定在最高的非 boost 档，
  没有动态调频，也没有温控。
- 调度器选空闲核时优先 A76。这不是大小核调度，已经在小核上运行的线程不会迁过去。
- USB 驱动的 `udelay` 改成原地等待，不再让线程睡眠。它现在真的按微秒计时，APFS 刷盘可能超过
  libpayload 的 5 秒处理上限，所以上限放宽到 60 秒。

照片（屏幕证据）：

- 升级页从“Press home to upgrade”变成“Swipe up to upgrade”，底部出现 Home 指示条。
- 第 7 次拷帧（2160×1440×4，12441600 字节）用了 16142 µs，距上一次 333 ms。此前未缓存映射、
  单核约 1.5 GHz 时一次约 0.23 s，帧间隔 5～6 s。写合并映射和固定最高频是同时上的，各自的贡献
  没有分开测。
- 稍后的周期报告：共 8 帧，10 秒内没有新帧，最后一次拷贝 193688 µs，距上一次 29903 ms。升级页是
  静止画面，不出新帧本身不说明慢；同一块缓冲第 8 次比第 7 次慢十倍以上，原因未查。
- `COACHZ freq`：A55 从第 6 档 1516 MHz 升到第 9 档（共 9 档）1804 MHz；A76 开机已在最高的
  第 13 档 2400 MHz；L3 从第 7 档 1209 MHz 升到第 10 档 1459 MHz。
- 内核 `memcpy` 约 4511 MB/s（这一行在照片底部，只拍到一部分）。
- 八个核都是步骤 7、PSCI 返回 0、EL1；CPU0～5 为 `p805 E`，CPU6、7 为 `p804 P`；探测仍是
  `grp1 0xff`。CPU6、7 的定时器和 SGI 计数（7188／39708、11079／40661）是各核中最高的，
  A55 为 1373～3277／10934～18628，大核已经在承担负载。
- 第一个 SpringBoard 仍以 SIGKILL 退出（namespace 2、code 9），退出时停在与 lsd 主线程相同的
  等待处；之后启动的 SpringBoard 画出了升级页。这次照片里没有拍到发出 SIGKILL 的进程。

仍然没有输入设备，升级页无法继续。

### 每核负载、实际频率与 CPU 占用（k36）

k36 只加测量（`coachz-prelink-measure.patch`），每 10 秒一组 `K36` 报告：每核空闲时间算出的忙碌比例；
把测量线程依次绑到每颗核上、关中断跑 32 条相互依赖的加法（A55、A76 都是每周期一条），得到实际频率；
cpufreq-hw 两个 CPU 域偏移 0x704 的当前投票；窗口内的拷帧统计；占用 CPU 最多的进程和 backboardd 线程。
出第一帧后屏幕控制台只用最上面 14 行（`coachz-console-band=14`），panic 时恢复整屏。

第一版把 0x18323000 起只映射了 0x3000，A76 状态寄存器在 0x2800 + 0x920，越界后在 cpu 7 上 data abort；
映射改为 0x4000 后正常。照片（屏幕证据）：

- 三次测量都是 CPU0～5 为 1804 MHz、CPU6、7 为 2399～2400 MHz，旧的 subs/b.ne 循环在每颗核上给出同样的数。
  k29 那次约 768 MHz 的读数是在拉高频率之前，当时的实际状态已无从确认。
- 两个 CPU 域的投票寄存器都是 `0xfff009c`，低 10 位 0x9c × 19.2 MHz ≈ 2995 MHz，高于实际频率，没有被压频。
- 开机 71 秒的 10 秒窗口里，占 CPU 最多的是 analyticsd（约 2.0 s）、mobileassetd（1.5 s）、dasd（1.3 s）、
  assistantd（1.2 s）和 launchd（0.9 s）。
- 开机 134 秒、升级页已显示时，12 秒窗口的忙碌比例为 CPU0 20%、CPU1 3%、CPU2～5 各 1%、CPU6、7 各 5%，
  合计约 0.37 颗核；占用最多的 ReportCrash 也只有 383 ms。这段时间没有新帧，最后一次拷贝用了 193270 µs。
- backboardd 仍在 92 秒对第一个 SpringBoard 发 SIGKILL。

因此频率是真实的，A76 能被调度并在承担一部分工作，但此时系统基本空闲，瓶颈不在 CPU 算力或大小核分配。
同样大小的一帧有时 16 ms、有时约 193 ms，原因未查。backboardd 线程一栏为空：它有 65 个线程，而记录表只有
32 项，超出的线程算不出增量，这是测量代码的缺陷。


### 帧拷贝、SpringBoard 被杀与锁屏（k37～k41）

k37 起报告还包括：每个 USB 块 I/O 的次数、字节、持锁和等锁时间；按内核栈分组的 backboardd、SpringBoard、
lsd 等待线程（栈帧从线程的 `machine.kstackptr` 读，无栈时显示 continuation）；拷帧前用 `AT S1E1R` 读出的
源缓冲区内存属性，以及拷贝前内容扫描的耗时。照片（屏幕证据）：

- k37：开机 101 秒的 10 秒窗口里，拷贝 15.5～170 ms，但拷贝前的两遍内容扫描用了 292 ms，源缓冲区属性
  为 0x44（不可缓存）。扫描和拷贝都在 backboardd 提交帧的线程里进行，这 10 秒 backboardd 用了 3.2 s CPU。
  USB 盘 10 秒内读 21.6 MB、写 4.6 MB，持锁 523 ms，不是瓶颈。
- k38 在显示补丁里关掉内容扫描（`scanBudget=0`，结果只用于诊断输出）：拷贝稳定在 20～21 ms，
  不再出现 170 ms。源属性读数变为 0x100（地址转换失败），因为不再先扫描时，源映射在拷贝开始前还没建好页。
- 第一个 SpringBoard 仍在约 90 秒被 backboardd SIGKILL，所以与扫描无关。k35 的 SpringBoard 栈停在
  `-[BKSSystemShellService _checkIn]`，即同步 XPC 向 backboardd 报到。
- k39 在 kill() 里记录 backboardd 的现场：发出 kill 的是主线程主队列上的一个 block（`x16 = 37`）；同时一个
  线程在做 LaunchServices 目录扫描并读文件，另一个在 `os_state` 处理里 `dispatch_sync` 等队列。
- k39、k40 两次的返回地址相差同一个偏移，据此在 backboardd 二进制里定位：block 属于
  `-[BKSystemShellSentinel _lock_handleRelaunchRequestFromSystemApp:withOptions:]`，日志依次是
  `Handling exit request from %@ with options: %@`、`Blocking system apps.`、`Killing %@.`。也就是说，
  backboardd 是按某个系统应用的重启 shell 请求杀掉 SpringBoard 的，不是看门狗超时。请求方还没看到。
- 之后的 SpringBoard 画出 "Swipe up to upgrade" 页，底部有 Home 指示条。

k40 在 launchd 之前向外接 Data 写入 UniXNU `scripts/seed-data-skip-setup.py`（来自 ios-sweet
`tools/sweet-root-tweaks.py`）的偏好：springboard 的 `BKNoWatchdogs` 等、migration 的 `LastSystemVersion`
与 `DMUserDataDisposition`、purplebuddy 的 `SetupDone` 等（已含标记键的文件不动，否则备份为 `.coachz-prev`
后替换，mobile 0600）。kill 和页面都没变，这在意料之中，因为 kill 不是看门狗造成的。
k41 按 ios-sweet NOTES.md 的结论给 `/chosen` 加上合成的 `unique-chip-id` 与 `chip-id`（0x8011）：
XNU 只用它们生成 IOPlatformUUID，没有它，CurrentHost 偏好写不进去，迁移就记不住已完成。kill 和页面仍然一样。
机主重启过，第二次开机结果相同。

这一页其实不是 SpringBoard 的锁屏，见下一节。偏好和芯片 ID 的效果仍未验证。

## 持久日志、触摸和 PreBoard（k42～k46）

系统从未卡死。k37～k44 的照片时间相近（如 `now 101 s`），是因为机主在同一个故障阶段结束实验；曾把这个时间读成
“100～120 秒卡死”是错的，k44 据此加的 `coachz-diag` 开关只留作减少输出。屏幕和诊断文字一直在刷新。

- k45 把屏幕控制台的全部输出每秒写进外接盘末尾 64 MiB 的日志区（分区和备份 GPT 之外，写入只来自内核的这一个线程），
  Debian 上用只读脚本取回。真机读回 166 KB，内核心跳到 148 秒还在走。之后不用再靠照片读日志。
- k42 起 GENI I2C 通了：SE4 上读到 HID 描述符 `vid 27c6 pid 0e94`，报告描述符 1193 字节，与 Linux 一致；
  ShengHidInput 的数字化仪挂到 IOHIDResource 下，报告全部送进 HID，错误数 0。上滑时原始 y 从约 8600 减小，
  x 沿 2160 像素一边，方向不用翻转。
- 但 k42～k45 的 `user clients` 一直是 0。k45 日志里有
  `DK: AppleUserHIDEventDriver-... waiting for server com.apple.driverkit.AppleUserHIDDrivers-...`，而这个 dext
  进程从未 exec。内核自带的数字化仪 personality 带 `IOMatchDefer`，要等 driverkitd；driverkitd 加入 OS 卷上
  `com.apple.AppleUserHIDDrivers.dext` 的 personality（`IOProbeScore` 1），由它接管设备，XNU 最多等 120 秒。
- k46 在发布设备前把一条内核 `IOHIDEventDriver` personality 加进 IOCatalogue（`Transport` SPI、Digitizer/Touch
  Screen、不带 `IOMatchDefer`、`IOProbeScore` 1000）。真机日志：驱动是 `IOHIDEventDriver`，`user clients 1`，
  没有 `DK:` 行；一次开机 141 秒内 210 帧。**机主从底边上滑，页面进入 “Enter Passcode” 并画出键盘（照片）。**
- 这一页来自 PreBoard：`PREBOARD_SWIPE_FOR_UPGRADE` = "Swipe up to upgrade" 在 `/Applications/PreBoard.app` 的
  字符串表里，它有 `_handleEdgeSwipe:` 和 `exiting as result of successful unlock`。日志里 PreBoard 启动后几行，
  backboardd 就杀了第一个 SpringBoard，并新起一个。backboardd 里有 `BKAlternateSystemApp`，据此推断这是在把系统界面切给
  PreBoard，请求方仍没有直接证据。
- 同一份日志里 `keybagd` 报 `FATAL KEYBAG ERROR: failed to load system bag`：内核里没有 AppleKeyStore。
- 拷一帧的时间按核分成两档（真机）：A76（CPU6、7）约 20 ms，A55（CPU0～5）约 209 ms，刷新慢和它有关，原因还没查。

## 主屏幕（k47、k48）

- k47 接入 UniXNU 的 `AppleKeyStoreState`（`coachz-aks=1`，启动代码改编自 UniXNU-sheng 的 `KeyStoreProbe.cpp`）：一个回答
  “没有 keybag、没有密码、没有锁定”的 AppleKeyStore，不提供任何密钥和保护。真机日志：`keybagd` 不再报
  `FATAL KEYBAG ERROR`，PreBoard 不再启动，第一个 SpringBoard 不再被杀。这说明之前的 SIGKILL 很可能和 PreBoard 有关。
- 随后屏幕出现苹果 logo 和进度条，由 `com.apple.datamigrator` 和 `com.apple.migrationpluginwrapper` 驱动。约 56 分钟里
  连续出帧（每 10 秒约 380 帧，拷贝基本落在 A76 上，约 21 ms 一帧），3372 秒后停止出帧；第二个插件进程到日志结束
  都没有退出，SpringBoard 也一直没画界面。
- `datamigrator` 的字符串显示触发条件只有两个：哨兵 `/private/var/datamigrator/migrate` 存在，或
  `LastSystemVersion` 与当前版本不同。之前用 `DMUserDataDisposition` 判断偏好已写入，而这个键 DataMigration 自己也会写，
  所以 `LastSystemVersion` 很可能一直没写进去。
- k48 改用 `LastSystemVersion` 判断，并在 launchd 之前删除哨兵。**机主照片：iPadOS 主屏幕、Dock 和状态栏都画出来了，
  没有再跑进度条。** 日志确认 `com.apple.migration` 被改写（原文件确实没有 `LastSystemVersion`），哨兵文件本来就不存在。

## App（k49～k52）

- 带云朵的图标是占位符：OS 卷的 `/private/var/staged_system_apps`（231 MB、29 个 App）被 Data 种子脚本排除了，当时种子只有
  64 MiB。Data 现在有 8 GiB，但这些 App 正常靠跳过的迁移安装，还没做。
- k49 的 `OS_ACTIVITY_DT_MODE` 没有把 `os_log` 镜像到 stderr，只录到 objc 警告和 CoreImage 的少量输出。系统日志还拿不到。
- k50 把崩溃现场和主线程调用栈写进日志区（`@crash`、`@sample`），符号化后：
  - `iconservicesagent` 崩在 `CI::GLContext::init` 调用地址 0。没有 GPU 时 CoreImage 仍走 OpenGL（`CIGLIsUsable` 恒返回 1），
    函数表是空的。UniXNU-sheng 的代码注释记录了同样的崩溃。
  - Settings 主线程在 `-[PKIconImageCache imageForKey:]` 的 `dispatch_sync` 里等，进程在，界面出不来。
  - backboardd 的 SIGBUS 是 `dc zva` 对齐错误（`esr 0x92000061`），位置在 `CA::OGL::SWContext::set_surface` 的 `bzero`。
  - Safari 的 WebKit 进程被 AMFI 启动约束拦下（`c[1]p[1]m[1]e[4]`），`coachz-no-launch-constraints=1` 放行后不再因此退出。
- k51（`coachz-null-call=1`）让 `iconservicesagent` 调到地址 0 时按返回 0 继续：29 次空调用都接住了，不再崩溃，并打印
  `Failed to render 3364 pixels because a CIKernel's ROI function did not allow tiling`。**Settings 的主线程随后越过图标，走到
  `-[PSGAboutController viewDidLoad]`**，停在 `_macAddress` 的 `dispatch_once` 上。所以 Settings 这条因果链成立，“关于本机”页还会卡住。
- k51 把不缓存的 DRAM 从 Device-GRE 改成 Normal 非缓存，backboardd 仍在同一条 `dc zva` 上 SIGBUS，所以这一改动没有解决它。
  k52 在内核里接住这个错误、用普通写入清零并记录 MMU 属性（`@dczva`），还没有真机结果。
- 机主报告 backboardd 崩溃重启后触摸没有反应。日志里报告仍在进 HID（`to HID 911`），`user clients 1`，SpringBoard 主线程空闲在 runloop，
  所以断点在 backboardd 重启之后的上层，原因没查。k49 那次机主还看到过一次 panic，内容没有落盘；k50 起 panic 文字也写进日志区，这段写入还没有真机触发过。
- k52、k53 的 `@dczva` 记到 `attr 0x0`：IOSurface 的 `VM_WIMG_RT` 映射在这个配置下就是 Device-nGnRnE（没有 `HAS_UCNORMAL_MEM`，
  `CACHE_ATTRINDX_RT` 等于 `CACHE_ATTRINDX_DISABLE`），k51 改的分支没有走到。接住 `dc zva` 之后，backboardd 又在 CoreGraphics 的非对齐访问上 SIGBUS。
- k54 让内核管理的内存（`pa_valid`）在这些不缓存映射下都用 Normal 非缓存。**真机日志：全程没有 `@crash`，`@dczva` 为 0，backboardd 和 SpringBoard
  都没有重启，Safari 和设置都运行着。** USB DMA 池和 MMIO 在管理窗口之外，映射不变。
- “设置 → 关于本机”仍然打不开：主线程在 `-[PSGAboutDataSource _macAddress]` 的 `dispatch_once` 上等，没有崩溃。还没有 Wi-Fi 驱动。

## 电池、测试音和音量键（k53、k54）

驱动由另一个会话写好，说明在 `docs/plans/battery-speaker.md` 和 `docs/plans/input.md`。

- 电池：`COACHZ battery: 97 % 8466 mV 0 mA 3474/3578 mAh flags 0xb ac 1`，随后 `published`，机主确认状态栏显示电量。同一时间 Debian 的
  `sbs-12-000b` 读到 97%、8464 mV、0 mA、3474/3580 mAh，充电器在线。
- 测试音：k53 失败在 `no contiguous DMA buffer`，因为内核内存全在 4 GiB 以上，而 LPASS DMA 只有 32 位。k54 把缓冲放到内核窗口外的低端 DRAM
  `0x81200000`：`tone played: 2 periods, 0 errors, irq 0x8, 1001 ms`，**机主听到了声音**。这只是一次性测试音，系统和 App 还没有音频输出。
- 音量键：`EC buttons supported 0x7, enabled volume`，按键时 `buttons 0x4 -> report 0x8: 0x0`、`0x2 -> report 0x4: 0x0`，报告进了 HID 且返回 0。
  屏幕上没有出现音量条，可能因为系统还没有音频设备，未验证。

k54 起第一帧之后屏幕不再显示诊断文字（`coachz-console-band=0`），输出只进日志区，panic 时恢复显示。

## 背光和键盘（k55）

- 背光（`coachz-backlight=1`，说明在 `docs/plans/backlight.md`）：`initial duty 52428`、`service conforms 1 attached 1 started 1`，
  随后 269 行 `target … applied … readback …`，读回值和设定值只差量化误差，错误数 0。**机主确认控制中心的亮度滑块能调屏幕亮度。**
- 键盘（`coachz-usb-keyboard=1`，`coachz_usb_hid.c`）：`keyboard at 2, vendor 18d1 product 504c, interface 0 ... protocol 0 idle -6 queue 1`。
  SET_IDLE 失败（-6），打字不受影响。**机主确认键盘能用。** 开机早期有一行 `the keyboard device is not ready for report 0`，因为报告先于 HID 设备到达。
- 这次开机全程没有 panic 和 `@crash`，系统盘正常。

## Adreno 618 前几级（k56～k59）

路线和寄存器依据在 `docs/plans/gpu.md`。每级都正常开到主屏幕，电池、键盘和测试音不受影响。GPU 还没有执行任何命令，界面仍是 CPU 渲染。

- `coachz-gpu=1`：`mode 1 stage 1 restored 0: ok`。GPLL0 已锁定（`d0100005`），cfg-ahb 在运行（`00000009`），CX GDSC 未上电（`00222001`），cx-gmu 关闭（`80004220`）。
- `coachz-gpu=2`：`mode 2 stage 7 restored 1: ok`，轮询 CX 1 次、时钟 2 次。窗口内 CX 上电（`f8222000`），GMU 寄存器可读，CM3_SYSRESET 为 1。撤销后只有 RCG cfg 和 cx-gmu 的 wake/sleep 字段保持改动，符合计划。
- `coachz-gpu=3`：32768 字节的 `a630_gmu.bin` 写入 TCM，8192 个字读回，0 处不一致，FNV-1a `0x51c28be8` 和按原文件算出的相同，CM3 始终在复位。固件取自 Debian，不进仓库，打包时用 `--gmu-firmware` 传入。
- `coachz-gpu=4`：只读 GPU SMMU。sCR0 `00000001`（CLIENTPD=1，旁路），IDR0 `78016606`，IDR1 `20008008`，6 个 stream 组，8 个 context bank，页 4 KiB，bank 从 +0x8000 起。SMR 和 S2CR 全为 0，每个 bank 的 SCTLR、TCR、FSR 全为 0，CBAR 都是 `00010000`。固件没有留下任何配置。

## 温度、Wi-Fi 第一步和加速度计（k60、k61）

都是只读探测，说明在 `docs/plans/thermal.md`、`wifi.md`、`motion.md`。

- 温度：k60 按 TRDY bit 0 门控，bank 0 一直是 `0x8`，读不出任何值，bank 1 也偶尔掉到 `0x8`。k61 改为只看每路的 VALID 位（和 Linux 一样），两组共 25 路全部有效：
  开机时约 37 °C，有负载时 bank 0 峰值到约 55 °C。Debian 同一时期 `cpu*-thermal` 约 44～49 °C，量级一致，但两边没有逐路对照。这还没有接入温控，频率仍然固定在最高档。
- Wi-Fi：`SMEM v12 inventory status 0`，14 个分区项，global 和 apps-modem 分区都解析到了。SMP2P 项 428、GLINK 项 478～480 都不存在，因为基带没有启动。没有启动 WCN3990，也没有发布网卡。
- 加速度计：EC 枚举出 3 个传感器。传感器 0 是 BMI 加速度计，12.5 Hz、±4 g，静止时读数合成约 1 g，错误数 0。陀螺仪 ODR 为 0（未启用），传感器 2 的类型 5 这个驱动不支持。没有接 CoreMotion，屏幕不会自动旋转。转动机身时重力依次落到 +Y、+Z、-X 轴，幅值一直约 1 g。
- 合盖（k62，`coachz-switches=1`，说明在 `docs/plans/switches.md`）：`supported 0xf raw 0x5`，bit 0（LID_OPEN）在 `0x5`、`0x4`、`0x5` 之间变化了一次，错误数 0。机主依次做了合盖和拆底座，而日志在开机 127 秒、这两次变化之后就停了，bit 2（BASE_ATTACHED）没有变化。所以不能认定 EC 漏报了底座；日志在拆底座前后为什么停止还没有查明。这只读取状态，不触发锁屏或睡眠。

## 系统音频（k63）

`coachz-speaker=2`：内核启动 `CoachzAudio` 服务，launchd 之前把 `CoachzAudio.driver` 写到 Data 卷，并用 bindfs union 挂到 `/System/Library/Audio/Plug-Ins/HAL`。插件的 CDHash 放在单独的 trust cache 模块里，打包时用 `--root-trust-cache` 传入。

- 机主反馈：系统声音正常，“声音完美”。这是听感判断，没有录音或频谱对照。
- 日志：`plug-in mount stage 3 status 0`，host clock 请求返回 0，输出开关 5 次、累计约 11.8 秒，校正 0 次，没有 `@crash`，没有 panic。
- k63 的路由显示为 “Speaker” 和音响图标。插件把设备报成 Thunderbolt，因为内置设备只能走 Apple 的端口描述，USB 设备又要带 HAL 不接受的位置信息（UniXNU-sheng 的结论）。k64 把设备名改成 “iPad” 后，机主确认名字变了，图标还是音响。
- 没验证：麦克风输入（输入环目前是静音）、音量条、耳机插孔、均衡和扬声器保护。

## 启动图（k64）

探针加载内核时，在黑底上居中画 `boot/splash_data.h`（512×512，由 64×64 放大）。开始清内存窗口前只显示红色通道，内核段复制完叠加绿色，ADT 和 RAM 盘复制完叠加蓝色，变成全彩。机主确认实际按 R、G、B 的顺序显示。它取代了原来的绿角和三个彩色方块。

图是机主 atm10-zh-cn 项目的资源包图标，已经带透明通道，叠到黑底上就是去掉背景。换图用 `tools/splash_image.py`，它也会把和边缘或透明区相连的近白色区域去掉，图内部的白色保留。

## GMU 固件启动（k65）

`coachz-gpu=6`，说明在 `docs/plans/gpu.md`。这是 CM3 第一次真正运行，日志原文的摘要：

- SMMU：11 个地址转换全部正确，8 个映射的地址翻到预期的物理地址，3 个未映射的返回故障。sCR0 运行时 `0x1812`，结束后回到 `0x1`，gfsr 0，fsr `0x400`（只有 FORMAT 字段）。
- RSC：写完序列后停止用 3 次轮询，唤醒 2 次，结束时再次停止 2 次。
- 固件：32768 字节写入 TCM，读回 FNV `0x51c28be8`，0 处不一致。版本字为 0，所以按 `0xbabeface` 判断。放开 CM3 后第 2 次轮询（约 200 µs）就读到 `0xbabeface`。
- 放开之后：FW_BUSY 0，RPMH 电源状态 `0xf`，AO 和 GMU2HOST 中断都是 0。SMMU 的 FAR 和 FSYNR0 都是 0。日志区、HFI 区和 dummy 页的第一个字都是 0。也就是说，这 200 µs 里固件没有产生可见的内存访问，这不能说明它的写内存路径已经通了。
- 收尾：CM3 回到复位，`mode 6 stage 7 restored 1: ok`。GCC 和 GPUCC 回到启动前的值，RCG 配置和 cx-gmu 的 wake/sleep 字段除外。系统照常运行到日志结束（144 秒），没有 `@crash`。

没有点亮 GX，没有发送任何 HFI 消息，GPU 核心没有执行任何东西，渲染仍然在 CPU 上。

## GX 上电和 HFI（k66）

`coachz-gpu=7`，说明在 `docs/plans/gpu.md`。

- CMD DB：`gfx`、`mx`、`cx` 各 16 档。800 MHz 对应等级 384，投票 `0x1800307`，即 GX 索引 7、MX 索引 3。
- `BOOT_SLUMBER` OOB：第 8 次轮询（约 800 µs）确认。INTR_INFO `0x40000000`，GX GDSCR `0xf8222000`（已上电），RPMH 电源状态 `0xf`，SPTPRAC 状态 `0x28`。
- SPTPRAC：第 1 次轮询就绪。HFI 队列：第 2 次轮询启动。
- HFI：`GMU_INIT`、`FW_VERSION`、`PERF_TABLE`、`BW_TABLE`、`TEST` 5 条全部成功，状态 0，固件版本 `0x10060000`，没有固件错误。固件能读到命令队列并写回响应，所以这一次证明了它经 SMMU 访问内存的路径。全程 gfsr 0，fsr `0x400`（只有 FORMAT 字段），FAR 和 FSYNR0 都是 0。
- `GPU_SET` OOB：第 2 次轮询确认。INTR_INFO `0x01000000`，RPMH 电源状态变为 0。
- 收尾：SPTPRAC 关闭用 2 次轮询，4 个 TCS 全部空闲，CM3 回到复位，`mode 7 stage 7 restored 1: ok`。GCC 和 GPUCC 回到启动前的值，RCG 配置和 cx-gmu 的 wake/sleep 字段除外，GX GDSCR 回到 `0x00222001`。系统照常运行到日志结束（127 秒），触摸有报告，没有 `@crash`。

没有碰 GPU 核心寄存器，没有写 SQE 固件，CP 没有启动，GPU 没有执行任何命令，渲染仍然在 CPU 上。GPU 频率也没有设置，投票值只是电压档。

同一次在 Debian 上只读确认：这台的设备树写着 `qcom/sc7180-trogdor/modem/{mba,qdsp6sw}.mbn` 和 `qcom,gsi-loader = "modem"`，文件都在，MPSS 运行中，`wlan0` 已启动。这是后面 Wi-Fi 的对照基准。

## CP 第一次执行命令（k67、k68）

`coachz-gpu=8`，说明在 `docs/plans/gpu.md`。

k67：GBIF、安全寄存器、地址模式和 63 项时钟门控都写进去了，随后 kernel panic，`Unaligned kernel data abort`，FAR 落在 GPU 窗口 +`0x3814`，也就是 `UCHE_WRITE_RANGE_MAX`。原因是我对这个不是 8 字节对齐的寄存器用了 64 位写。这也是 panic 文字第一次在真机上写进日志环。

k68（改成两次 32 位写之后）：

- DCVS：第 2 次轮询确认，返回 0，请求的是最低档 180 MHz。没有读回实际频率。
- SQE：`a630_sqe.fw` 版本字 `0x16ee207`（低 12 位 `0x207`），读回一致。
- ME_INIT：第 2 次轮询，读指针追上写指针。之后 `RBBM_STATUS` 为 `0x1`，只有 CP_AHB_BUSY_CX_MASTER，即 CPU 自己的访问，GPU 空闲。
- 从 ring 的 `CP_MEM_WRITE`：第 2 次轮询到达，值正确。
- 从 IB 的 `CP_MEM_WRITE`：第 2 次轮询到达，值正确。
- `CACHE_FLUSH_TS` fence：第 1 次轮询到达，值正确。
- `WHERE_AM_I` 影子：31，等于写指针 31。
- 故障：`RBBM_INT_0_STATUS`、`CP_HW_FAULT`、`CP_INTERRUPT_STATUS`、`CP_PROTECT_STATUS` 都是 0。bank 0 fsr `0x400`（只有 FORMAT 字段），FAR 和 FSYNR0 是 0，gfsr 0。
- 收尾：GBIF 三次停止都是第 1 次轮询确认，软复位后关机，`mode 8 stage 7 restored 1: ok`，GX GDSCR 回到 `0x00222001`。系统照常运行到日志结束（122 秒），没有 `@crash` 和 panic。

这证明 CP 能跑 SQE 固件，能经 SMMU 读命令并写内存。没有运行任何着色器，也没有画任何东西。跑完全部关掉，GPU 不留给系统使用，所以渲染仍然在 CPU 上。

## GPU 合成上屏（k69～k72）

四块（常驻驱动、Turnip、Metal、合成器）合并成一张镜像测试，说明在 `docs/plans/gpu.md`。

- k69：SMMU 工具包在路由 stream 时拒绝（全局配置不符），CP 没启动。同时预留 768 MiB，而 XNU 当时只配了 1 GiB，jetsam（namespace 1 code 4）杀光了进程，所以没进界面。插件 9 个文件写入，挂载成功。
- k70：内存窗口 4 GiB，约 3.95 GiB 可用页，进桌面，没有 jetsam。SMMU 工具包还要求空闲 stream 组先 fault。CPU 继续合成。
- k71：内存窗口 6 GiB，约 5.83 GiB 可用页。SMMU、ME_INIT、发布服务、`AGXAccelerator` 全部通过，基类地址和审计记录一致。合成器发现 Metal 后选了 `b3a8` 表面，显示驱动拒绝，帧计数为 0。
- k72：ADT 改成 sRGB。**机主看到了界面，日志确认由 GPU 合成：** 开机约 112 秒内 ring 提交 400 次，fence 400/400；199 帧交给 GPU，198 帧完成，失败 0、放弃 0；CPU 拷贝帧数 0。RBBM、CP、protect 都是 0，bank 0 fsr 只有 FORMAT 字段，gfsr 0。没有 `@crash` 和 panic。

k72 的性能数据（插件自己的统计，355 MHz）：
- 提交到完成 374 次，平均 40 ms，最长 101 ms。
- 有内容的帧约 9 Mpix，是屏幕的 3 倍。
- 首次运行编译 83 条管线，共 5.2 秒，最长一条 397 ms。缓存 hit 30、miss 112。
- 温度峰值 45 °C（TSENS），时长只有约两分钟。

已知问题和没验证的：
- 屏幕控制台的文字盖在界面上（跑码）。它靠第一帧 CPU 拷贝来静音，GPU 路径没有这一帧。
- 机主反馈帧率不高。没有给内存发带宽投票，也没有动态调频。
- 还没有确认 App 自己能用 Metal。放行的进程只有 backboardd、iconservicesagent、WidgetRenderer_Default 和 PosterBoard。
- GPU 没有电源管理（GPU_SET 一直保持）、没有中断、没有复位和每进程页表。长时间稳定性没有验证。

## GPU 帧率和 EC 探针（k73、k74）

k73：GPU 升到 650 MHz，GPU 路径接手时也静音屏幕控制台。k74：Turnip 每个 pass 都用系统内存渲染（`TU_DEBUG=sysmem`），另外加了 GPU 时钟/忙碌计数和 EC 的只读探针。两次都进桌面，没有 `@crash` 和 panic。

- 跑码：机主确认已经消失。
- k73：ring 提交 1165 次，全部 fence；571 帧上屏，失败 0。提交到完成平均 32.0 ms（k72 在 355 MHz 下是 40.0 ms），最长 180 ms。
- k74：ring 提交 1943 次，全部 fence；945 帧上屏，失败 0。提交到完成平均 28.2 ms，最长 330 ms。
- 时钟：CP 的 always-count 计数器对 always-on 计数器，每秒都是 649～650 MHz，和设定一致。
- 忙碌率：GMU 的 busy 计数器（Linux `a6xx_gpu_busy` 用的那个）最高 56%，GPU 将近一半时间空闲。
- 重帧（插件 FA3）：间隔 58.8 ms，其中 GPU 段 34.1 ms（k73 是 39.0 ms），CPU 编码 25.8 ms（k73 是 25.9 ms）。渲染服务器编码完一帧就同步等它完成，再编码下一帧，两段是串行的，所以间隔约等于两者之和。每个命令缓冲从提交到被调度（运行时翻译成 Vulkan 再提交）要 5～8 ms，只有一次 blit 的也一样。
- 画面：机主看到电池、时钟这类小图标偶尔会花。两次日志都有同一条警告：一张约 95×29 的 A8Unorm 纹理建在 no-copy buffer 上，运行时没法为它建线性图像。花屏是不是 sysmem 引起的，还不知道。
- 内存（k73，开机 40 秒后在 CPU0 和 CPU6 上各测 32 MiB，循环和 Debian 上的基准相同）：XNU 比 Debian 慢 10～30%，没有数量级差别。

EC 探针（k74，`coachz-battery=1`）每 10 秒读一次两个 PD 端口的 `EC_CMD_USB_PD_POWER_INFO`，再按 Linux `cros_ec_debugfs` 的方式读 EC 自己的控制台（`CONSOLE_SNAPSHOT` 加 `CONSOLE_READ`），全部写进日志环，不改 EC 的任何状态。

- k73 里机主重新插拔充电器后，充电停了，灯也灭了。EC 的 PD 日志（Debian dmesg）：谈到 5V 3A 后约 60 ms 断开，之后 EC 把这个端口记成供电方，输入限流 512 mA，没再升到 12V。
- 同一个充电器在 Debian 下重新插拔，约 0.4 秒就到 12V 2.5A，开始充电，所以充电器和电池没有问题。
- k74 里重新插拔一次，结果正常。EC 控制台依次是 `Attached.SNK`、5V 3A、请求 12V 2.5A、`PE_SNK_Ready`、`AC on`，前后约 0.2 秒，电池恢复充电。k73 那次的原因不明，没有复现。
- EC 控制台还有 139 次 `MKBP not cleared within threshold, toggling`：XNU 只轮询按键状态，没有像 Linux 那样把 MKBP 事件取走。这次带着它照样正常充电。

## 新版 GPU 上层（k75）

Turnip、metal2vulkan 运行时和 Metal 插件改从固定在新版本的 UniXNU 副本构建（`upstream.lock.json` 的 `unixnu-gpu`）。内核仍是 k74 那版 Adreno740 框架，补了新上层用的 `SetPriority`（只接收）和 `Note`（写进日志环）两个方法。sysmem 改由启动参数 `sheng-tu-debug=sysmem,nocb` 设置。

- 进桌面，ring 提交 532 次全部 fence，252 帧上屏，失败 0；没有 `@crash` 和 panic。
- 机主这次没有看到小图标花屏，也没觉得变快。
- GPU 忙碌率最高 72%（k74 是 56%），时钟仍是 650 MHz。
- 新插件的分帧统计和 `Note` 都要 `sheng-diag=1` 才记录，这次没开，所以没有可比的帧分解。
- 渲染服务器最忙的几条线程常在 A55 上运行，优先级多是 37。

同一次的照片：主屏小组件是白色空块，壁纸全黑，图标压在小组件上。面板 EDID 是 2160×1440、233×155 mm（3:2，方形像素），而系统装扮成 11 英寸 iPad Pro（约 1.43:1），启动时的苹果标志被横向拉宽。

## 屏幕方向、GPU 剖析、MPSS 前提（k76～k78）

k76～k78 都带 `--diagnostics`：插件分帧统计（`sheng-diag=1`），重建后的 MobileGestalt 缓存以十六进制写进日志环（`coachz-dump-mgcache=1`），以及 locationd、backboardd、SpringBoard 的 IOKit 查询和服务打开记录（`coachz-match-log=1`）。三张都进桌面，没有 panic 和 `@crash`。

- MobileGestalt：屏幕答案是真实面板，竖向记为 1440×2160、2 倍（`oBbtJ8x+…`），另有一对 double 2160.0、1440.0。`ArtworkDeviceSubType` 是设备树给的 2388。
- k77 把机型改成 iPad mini 6（iPad14,1、2266），缓存里确实变了，但主屏布局和 k75 完全一样，启动 logo 横向拉宽约 2.2 倍。原因不在机型：主屏是 4 列，即竖屏网格。没有方向输入时 iPadOS 按竖屏排版，而这块面板原生是横的，竖屏的版面和 logo 就直接画进了横屏里。机型改回 11 英寸 iPad Pro。
- k78，locationd 在设备树里按节点名找 `gyro` 和 `pressure`，这台没有，所以 CoreMotion 不会认为有运动传感器。backboardd 的设备方向来自 CoreMotion。
- k76 分帧：CPU 编码大多不到 4 ms，每帧 GPU 时间大多 32～128 ms。
- k78 GPU 剖析（GPU 忙时每秒，按核心周期计）：SP 忙约 40～55%，ALU 只有 4～15%，其余在等 RB 和 UCHE；UCHE 平均延迟 180～370 周期，TP L1 未命中 30～40%；GBIF 读写合计每秒约 3000 万拍，不到 2 GB/s（按每拍 32 字节估计）。是访存延迟，不是算力或带宽。
- k78 LLCC：切片 1、8、11、12 都是停用（status `0x2`），属性寄存器为 0；GPU 的 `SYSTEM_CACHE_CNTL_0/1` 也是 0。系统缓存在 XNU 下完全没配置。主线开机时会配置全部切片并激活 CPU 切片，GPU 启动时再激活 GPU 和 GPUHTW 切片。
- k78 MPSS：SMC64 的 SIP 调用可用。TF-A 没有实现 PAS 系列（init-image、mem-setup、auth-and-reset、is-supported、mss-reset），实现了 `assign_mem`。所以 modem 只能按主线 trogdor 的 MBA 自认证路线（q6v5 MSS）启动。TCSR halt：Q6 和 NC 空闲，没有 halt 请求，modem 没在运行。SMEM 清点照常通过。

## LLCC、前台优先、AOSS 和 RPMh（k79、k80）

- k79，LLCC 照主线配置（`coachz-llcc=1`，版本 1.1.2，1 个 bank）：CPU 切片（1）、GPU 切片（12）和 GPUHTW 切片（11）都已激活，属性值与 `llcc-qcom.c` 的 sc7180 表一致；GPU 的 `SYSTEM_CACHE_CNTL_1` 为 `0x16c6318c`，`CNTL_0` 为 `0x3`。和 k78 比：UCHE 平均延迟 268 → 215 周期，提交到完成平均 68.9 → 51.9 ms（约 −25%），每帧 GPU 时间的众数从 64～128 ms 区间移到 32～64 ms。CPU 内存探针没有可见变化。
- k80，GPU 提交按前台优先排队（backboardd 和报了高优先级的进程先走，其他进程在锁外最多等 16 ms）：整场只拦了 7 次、113 ms。插件统计里的“其他 GPU 作业”是渲染服务器每帧之后那个只有一次 blit 的小命令缓冲，从提交算到完成，主要是在 ring 里排在画面后面的时间，并不是别的进程的作业。提交到完成平均 57.7 ms，最长 142 ms（k79 是 273 ms）；两次操作内容不同，平均值的差别不能归因于这项改动。
- k80，AOSS QMP 通道照 `qmp_open` 打开：链路、通道、应答都是 UP。
- k80，AP 侧 RPMh：经 apps RSC 的第一个 active TCS 发出 `mss.lvl=0`，10 µs 内 issued 和 completed 都置位，IRQ 状态位置位。之后的电源轨投票和带宽投票走同一条路。
- k80，locationd 读了 `gyro` 节点的 `gyro-interrupt-calibration`（CFData，至少 8 字节：偏移 0 的 u16 版本、偏移 4 的 u16 dt、偏移 8 的 u16 rate），之后没有别的内核查询。运动数据走 HID 事件系统的快速通道（`IOHIDServiceClientFastPath*`），需要一个能提供快速通道的加速度计 HID 服务。

## modem 固件、GPU 调频、io_submap 和内存带宽投票（k81～k84）

- k81 第一次构建为 modem 固件申请了带 `Z_NOFAIL` 的 1 MiB `kalloc_data`，开机即 panic；改成不带 `Z_NOFAIL` 的 256 KiB 并检查返回值后正常。固件块在内核分区 144 MiB 处：`mba.mbn` 283296 字节、`qdsp6sw.mbn` 65583404 字节，sha256 都对，ELF machine 164（Hexagon），入口 `0x4417000` 和 `0x86000000`。
- k81，GPU DCVS 切档（`GMU_DCVS_PERF_SETTING` 加 HOST2GMU bit 23，等 GMU2HOST bit 31）：3、1、7 档分别在 60、60、28 µs 内应答，CP 周期计数器测得 354、179、799 MHz。800 MHz 比 650 MHz 没有更快，UCHE 延迟按周期数上升。
- k81，频率表：A55 288～1804 MHz（596～868 mV），A76 652～2400 MHz（588～956 mV）。PSCI 1.1：CPU_SUSPEND 特性 2（支持 OS 发起），SYSTEM_SUSPEND 支持，SET_SUSPEND_MODE 和 CPU_DEFAULT_SUSPEND 不支持，SMCCC 1.2。
- k82 让 Turnip 自选 tile 内存（`sheng-tu-debug=nocb`）：SP 忙从 48.6% 降到 26.9%，UCHE 延迟从 366 降到 274 周期，但提交到完成从 52.3 ms 升到 56.9 ms，所有颜色红蓝互换（照片）。UniXNU `porting/gpu` 的渲染模式一节早已记录同样的结论，k83 起回到 `sysmem,nocb`。
- k82 第二次启动 panic：`kmem_alloc(…, 589824, …) failed with 3`。589824 字节是 MBA 第一段整块映射 GCC 的大小；ARM 的 io_submap 固定 8 MiB，`ml_io_map` 的映射不释放，已经被各驱动的寄存器窗口占满。
- k82，温控自检：钳位时档位寄存器读回 A55 6、A76 9，GPU 上限 6；撤销后读回 9、13 和 7。0x704 寄存器始终是 2995 MHz，实际频率是否下降没有测。调频器在一个安静的秒里就从 800 降到 430 MHz。
- k83，io_submap 扩到 32 MiB（同 x86），K44 行打印用量：开机后 7420 KiB，原来的 8 MiB 再放 576 KiB 必然失败。Wi‑Fi 代码改为共用一个 4 KiB 的 GCC 页。没有 panic。
- k83，MBA 第一段：AOP 接受 load_state 开和关（返回 0）；cx/mx/mss 投到 15 档，40 µs 内三条命令 issued 和 completed（`0x10101`）；iface 时钟本来就在跑（`0x9`），bus、snoc_axi 从 `0x80000000` 变成 `0x1`，mnoc_axi、nav 从 `0x0` 变成 `0x1`；保持 2 s 后全部撤回，电源轨 10 µs 回到 0。
- k84，DDR 和内存 NoC 的带宽投票（`coachz-ddr-vote`，按主线 `bcm-voter.c` 的公式，峰值取 sc7180.dtsi 中 A76 2.4 GHz 与 GPU 800 MHz 的 8532000 kBps）：MC0（地址 `0x50000`，unit 2050000，width 16，VCD 0）y 截到 16383，ACV（`0x5000c`）使能位 8，SH0（`0x5003c`，unit 1100000，width 16，VCD 1）和 SH4（`0x50028`）y 为 7756；1270 µs 内四条全部完成。XNU 里原本没有任何人投这一票。
- k84，同一段测速循环在投票前后和 Debian 上：

| | 投票前 | 投票后 | Debian |
| --- | --- | --- | --- |
| A55 延迟 | 192～198 ns | 134～140 ns | 133～136 ns |
| A55 读/写 | 3.9～4.3 / 6.2～6.3 GB/s | 5.7 / 10.5～11.0 GB/s | 5.5 / 9.5 GB/s |
| A76 延迟 | 194～196 ns | 137～142 ns | 132 ns |
| A76 读/写 | 7.6～7.9 / 7.0～7.3 GB/s | 11.0～11.9 / 13.6～14.4 GB/s | 11.4～13.2 / 14.6～15.1 GB/s |

- k84，GPU 的 UCHE 平均延迟从 323 降到 247 周期，GPU 忙约 33～36%，帧时间几乎没变（前后操作不同）。插件的 commit-to-complete 是从 CPU 提交到用户态完成回调，不只是 GPU 执行：k83 某 10 s 里 227 个命令缓冲按 GMU 忙碌计数合计约 3.4 s GPU 时间，平均每个约 15 ms，而提交到完成约 70 ms。

## IO 一致性、着色器预热、帧拆分和 MBA 第二段（k85）

- UniXNU-sheng 的一致性探针在 A618 上通过：`H d64/64 a1 r1024/0 w1024/1024 s0 k6 -> on`（CPU 脏行 64/64，GPU 读到新值 1024、旧值 0，CPU 读到 GPU 写的 1024，DRAM 也是 1024）。打开 `native_adreno_io_coherent` 后，到约 200 s 提交前跳过约 43 万次、完成后跳过约 18 万次导入缓冲的缓存维护。提交到完成平均 24.8 ms（k84 约 52 ms），每个 render pass 的 GPU 时间平均约 1 ms。
- 帧拆分（`coachz-gpu-trace=1`，按 30 s 汇总 backboardd 的 frames.trace）：120～150 s 有 1308 个命令缓冲，编码 2.3 ms、提交到调度 12.9 ms、提交到完成 35.8 ms，5962 个 pass 共 6.05 s GPU 时间；交给显示的拷贝平均 30 ms（安静时约 17 ms），是现在最大的一段。
- 着色器预热：列表 158 个库，render server 约 170 s 开始，到 230 s 完成 30 个库、197 个 pipeline。
- 168～205 s 界面不响应：触摸报告照常进入 HID（每 10 s 400～580 次），画面保持每 10 s 100 帧、每帧两个小提交；前台应用和 SpringBoard 的主线程都停在 run loop 等事件；GPU 提交全部完成，没有 panic，约 208 s 恢复。原因未定。
- MBA 第二段（`coachz-wifi=4`，200 s）：apps SMMU `sCR0` 为 `0x200001`（CLIENTPD，旁路）；load_state、电源轨和五个时钟照常；`mba.mbn` 283296 字节拷进 mba_mem（首字 `0x464c457f`），assign_mem 交给 modem 返回 0。之后的 QDSP6SS/RMB 步骤到 233 s 都没有输出，线程停在某次寄存器访问上；系统其余部分继续运行。

## modem 的 Q6 启动、触摸中断、CPU 调频和 GPU 缓冲区（k86～k91）

- k86，MBA 第二段补上主线的一步：先把 mpss_mem（0x86000000，32 MiB）交给 Q6，再交 mba_mem。之后 Q6 的 sleep 和 XO 时钟、boot FSM 立即完成（MSS 状态 `0x9`/`0xb`），PBL 约 50 ms 报告成功（`0x1`）；MBA 状态 5 s（k91 延长到 15 s）内一直是 0，RMB 寄存器启动前全为 0、期间不变。收回内存后读到的 MBA 日志依次是：MBA Start、sec pil 初始化、BAM 初始化成功、“applying debug policy as secboot is enabled”、“Secure Device : 1”，之后没有输出。Debian 的固件目录没有 `msadp`，Linux 照常启动同一份 `modem/mba.mbn`。
- k86 和 k87，密集触摸（约每秒 100 份报告，一次轮询读一份，间隔 5–10 ms）之后，触摸控制器的中断线持续高电平几十秒，期间主动读取仍返回控制器上一份报告，之后自己恢复。k88 起中断线拉低时连续读到它回到高电平（最多 16 份），轮询间隔 2 ms，并在中断线 500 ms 没有拉低时每 20 ms 主动读一次、只把内容变化的报告交给 HID；k88 的密集触摸中断线一直正常。
- k85 的冻结经 k86 的每秒记录确认是触摸输入停住，系统本身仍在运行；k86 那次触摸停住时 GPU 正在以约 30 帧每秒渲染。
- k86，按进程统计：7202 次 backboardd 提交里只有 7 次前面排着别的进程的任务，每次最多 1 个；UniXNU 记录的“慢帧排在别的进程后面”在这里不成立。
- k86，GPU 调频改用 UniXNU `porting/power` 的规则（每 100 ms 选让 GPU 约一半时间忙碌的档位，不低于 3 档，触摸后 1 s 最高档，从空闲恢复至少 565 MHz），真机上在 3 档和 7 档之间切换。
- k87，CPU 调频（`coachz-cpufreq=4`，同一页的规则：每 16 ms 按最忙的核选频率，留 1.25 倍余量，连续 4 个窗口都更闲才降，触摸后 1 s A76 最高档，GPU 有提交时所有频域不低于最高档的 60%）：A55 平均 314–1804 MHz，A76 在触摸和 GPU 工作时多在 2400 MHz。温控钳位在调频器运行时只设上限。
- k89，panic：`pmap_assert_free`。一个用 GPU 的进程崩溃成 corpse，它的地址空间保留着 GPU 缓冲区的视图，而 `clientClose` 已把不在池里的缓冲区还给 VM。k89 的 512 MiB 池峰值用到 472 MiB。k90 把池扩到 1536 MiB（UniXNU 默认值），并对仍被映射的非池缓冲区延后释放；k91 照 UniXNU 的 `allocateSystem`，让池放不下的用户缓冲区用不属于任何 task 的可分页系统内存，客户端视图映射它自己的描述符。k90、k91 没有 panic，池峰值 216 和 88 MiB，系统内存缓冲区和延后释放都没有用到。
- k90，送显拆分：一帧渲染完成之后，拷贝到屏幕还要约 15 ms（送显请求到拷贝完成 22–25 ms）。
- 启动参数上限是 1023 字节；k87 去掉了已完成调查的 `coachz-dump-mgcache=1` 和 `coachz-match-log=1`。

## MBA 就绪、MPSS 认证、modem 运行和送显拷贝（k92～k95）

- k92，apps SMMU 只读：`sCR0` 为 `0x200001`（CLIENTPD，旁路），160 个 SMR、80 个上下文，没有有效的 SMR，14 个 S2CR 不在复位值，80 个 CBAR 都设过值。mba_mem 里读不到 Linux 上次留下的 MBA 日志；k93 起确认那里开头全为 0，和 MBA 成功时的状态 `0x2`（解锁并擦除）一致。
- k93，Debian 正在用的设备树里，LTE 版的 mpss_mem 是 0x86000000 起 140 MiB（主线 `sc7180-trogdor-lte-sku.dtsi`），加载的也是 LTE 版 `mba.mbn`（和 Debian 的 `modem/` 哈希一致）。之前交给 modem 的是非 LTE 的 32 MiB。改成 140 MiB 后，PBL 51 ms 通过，MBA 157 ms 报告就绪（状态 `0x2`）。IPA 的五个总线 BCM（SN9、SN0、SN1、SN12、CN0）和 IP0（100 MHz）投票都完成，状态 `0x10101`。
- k93，开机约 34 s，CPU 7 panic：`ArmCpu: unexpected architectural IRQ 3`。时间和核号都对应第一次 SMP 报告里那次一次性的 Group 1 SGI 自检：它给本核发 SGI 0–7，每个只轮询 400 次，在 A76 上留下了一个没收走的 SGI 3，而 ArmCpu 只认 SGI 0。k94 起去掉这次自检，ArmCpu 收到 SGI 1–15 只计数；k94、k95 计数都是 0，没有 panic。
- k94，照 `q6v5_mpss_load` 加载 `qdsp6sw.mbn`：元数据 1076 + 8104 字节 5 ms 认证通过；30 个可加载段（文件 65.5 MB，展开 144 MB）从 USB 盘 5.8 s 装完，MBA 58 ms 后报告认证完成，MBA 日志写着 “Received size greater than 32MB updating LTE” 和 “MBA Execution is successful”。15 s 内没找到 modem 的 SMP2P 条目，回收时 modem 的 AXI 口忙、不应答（k93 只跑 MBA 时是空闲的）。原因之一在这边：查 SMEM 条目时用的是开机清点时读到的分区末尾，之后新建的条目都看不到。
- k95，每次查找都重新读分区头之后：apps-modem 分区的已分配末尾从 `0x20` 涨到 `0x5208`，modem 运行并建了约 20 KiB 的条目。它没有置 ready 位；AP 这边自己的 SMP2P 条目（435）和 GLINK 都还没有，15 s 后照 Linux 的失败流程回收了 modem。
- 送显拷贝：GPU 把每一帧拷进固件帧缓冲（UniXNU 的 hand-off）。k94 时这一拷平均 24 ms（71 帧），从一帧渲染完到拷贝结束约 31 ms，而每个渲染 pass 的 GPU 时间平均约 2 ms；提交到送进 GPU 之前还在编码线程上等约 12.7 ms。k95 的拷贝核每个线程搬 4 个像素（16 字节读写），406 帧平均 11 ms、最近一次 9 ms，前 4 帧逐行比对无差异；等待拷贝完成的平均时间从 23.3 ms 降到 10.6 ms。
- 触摸：k94 的 48 份、k95 的 219 份报告全部送进 HID，0 错误。

## modem 报告 ready 和 handover、GPU 提交时间戳（k96、k97）

- k96 由 AP 先建了 SMP2P 条目 435。modem 照样跑起来，在 apps-modem 分区里建了 605（32 字节）、478（GLINK 描述符）、480（16 KiB 的 GLINK FIFO）和 611（4 KiB），但没有出现 428。主线 `smp2p.c` 把 `qcom,smem = <435>, <428>` 依次当作入站、出站：435 是 modem 写的，428 才是 AP 的。k95 没建任何条目时分区末尾同样是 `0x5208`，多出的 360 字节就是 modem 自己的 435；k96 占了它的位置。
- k97 由 AP 建 428（TCSR 互斥锁 3 号下分配，`master-kernel` 和 `ipa` 两项）。modem 把版本降到 1、带 SSR_ACK 协商成功，MPSS 认证完成后 30 ms 在 `slave-kernel` 里置了 ready 和 handover（`0x6`），照 Linux 在 handover 时撤回了代投的电压票。它运行约 40 s 后置 fatal，SMEM 里的原因是 `dog_hal_common.c:173:DOG detects stalled initialization, triage with IMAGE OWNER`。这时它已经建了 GLINK 的 478 和 480，在等 AP 的 479；AP 这边还没有 GLINK、QRTR 和 rmtfs。系统本身没有 panic。
- Debian 上 `qrtr-ns` 和 `rmtfs -r -s -v -o /lib/firmware/rmtfs` 在跑，EFS 是那个目录里的 `modem_fs1`、`modem_fs2`、`modem_fsc`、`modem_fsg`（各 2 MiB）。
- GPU 提交时间戳（`COACHZ gpu stamps`，CP_ALWAYS_ON_COUNTER，结束前先 CP_WAIT_FOR_IDLE）：GPU 执行一次提交约 2–6 ms，排队平时几十微秒、忙时平均约 3–4 ms，完成后约 0.1 ms 被看到。同时段渲染一帧“提交到完成”平均 13–19 ms，大部分不在 GPU 上。送显拷贝平均 8–10 ms，其中 GPU 执行约 1.5–2 ms。
- 触摸：k97 的 764 份报告全部送进 HID，0 错误；杂散 SGI 一直是 0。

## GLINK、QRTR、rmtfs 和 modem 的服务（k98）

- AP 这边按主线建了 GLINK（FIFO 479，交换版本，modem 的 features 是 `0x7`，协商为 `0x1`）。modem 依次打开 `glink_ssr`、`IPCRTR`、`SSM_RTR_MODEM_APPS`、`apr_voice_svc`、`apr_apps2`、`LOOPBACK_CTL_MPSS`、`DS` 等通道，只有 `IPCRTR` 由这边打开。
- QRTR：这边是节点 1，modem 是节点 0。收到 modem 的 hello 后通告了 rmtfs（服务 14）。modem 随后打开 `/boot/modem_fs1`、`modem_fs2`、`modem_fsg`，要了 2 MiB 共享缓冲区，读写了 8194 个扇区；EFS 是 Debian 的那四个全零镜像，只留在内存里。
- modem 通告了约 30 个 QMI 服务，其中有 69（WLFW，WLAN 固件服务）、3、5、9、10、11、12、15、17、21–24、34、36、43、47、49、51、66、74、76、4098。k97 那次 40 s 后的 “DOG detects stalled initialization” 没有再出现，120 s 时状态位仍是 ready 加 handover。
