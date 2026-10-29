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
