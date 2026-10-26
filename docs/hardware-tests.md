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

原生探针日志已取回。源码 XNU 入口检查点已完成主机验证和介质准备，真机执行待确认；探针显示恢复和异常日志接收仍需独立验证。


## 2026-10-27：源码 XNU 入口检查点介质准备

这是一轮介质准备记录，尚无检查点启动结果。使用真实源码构建的 XNU 10063.141.1，插入四字节 pre-MMU HVC 检查点，装载与返回门见 [入口检查点](xnu-checkpoint.md)。51 项主机回归与独立源码审查通过；原有四种探针仍可构建。

| 产物 | SHA-256 |
| --- | --- |
| 源码内核 | 6b406b8cd4ee5a6c11793256d8cadc5453365e8e8edb5f487953e02e259c7e9b |
| instrumented kernel | 49426594554a6e07164599c9514d69da340dde7e9dbdfa8b94075835fe718943 |
| linux-log.kpart | 3357afbe3d1a81ffeaadf669986eb27f09d804e9acd0e83c1efdd2e93ba67abb |
| linux-log.img | 5a385131f66934983e96e913b4f6e4bc9a39a79256a9da740238fcfafa5910ec |

目标 Debian 独立完成 vboot 签名、body、GPT 与传输哈希校验。写入已核对的可清空外置介质后，完整 128 MiB 回读 SHA-256 一致。写入器验证 USB／removable、容量、根分区、boot、swap 与独占打开；未写固件或内置盘，没有自动重启。

待从原厂开发者模式启动，再用 `read_probe.py --xnu-checkpoint` 验证同次引导日志。仅返回 Linux、保持黑屏或自动重启均不能单独证明 XNU 读取了 boot_args。
