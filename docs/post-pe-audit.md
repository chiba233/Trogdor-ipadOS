# 完整 PE 返回之后的初步审计

更新：2026-10-31。当前真机验收已由 version7 的完整 `PE_init_platform(FALSE)` 返回，推进到独立 version11 的 WFE／单 CPU 拓扑解析返回。version11 全部 40 个严格字段通过，完成状态核对与恢复后交接 Linux，见 [检查点契约](xnu-topology-checkpoint.md)。这里保留此前源码／实际生成码审计及后续线程／VM 依赖。

## 固定输入与边界

继续使用同一个源码内核，SHA-256 `6b406b8cd4ee5a6c11793256d8cadc5453365e8e8edb5f487953e02e259c7e9b`。检查对象是实际 BCM2837 非 Apple ARM64 构建及其对应工作源码，主要为：

- `osfmk/arm/arm_init.c`
- `osfmk/arm/machine_routines_apple.c`
- `osfmk/arm64/machine_routines.c`
- `osfmk/arm/rtclock.c`

当前 version7 将 `arm_init` 中 `0xfffffff0079b3cb4` 的原始下一条调用替换为 HVC，正常日志 ELR 为下一条位置。固定原内核的生成码顺序如下：

| 调用位置 | 原始调用 | 观察 |
| --- | --- | --- |
| `0xfffffff0079b3cb4` | `configure_timer_apple_regs` | 当前检查点停止于此，尚未调用 |
| `0xfffffff0079b3cb8` | `wfe_timeout_configure` | 计算 WFE 事件参数并写全局 |
| `0xfffffff0079b3cbc` | `wfe_timeout_init` | 实际读写 `CNTKCTL_EL1` |
| `0xfffffff0079b3cc0` | `configure_misc_apple_boot_args` | 此构建为空函数 |
| `0xfffffff0079b3cc8` | `configure_misc_apple_regs(true)` | 此构建为空函数 |
| `0xfffffff0079b3d48` | `ml_parse_cpu_topology` | 位于 chosen／stall 检查和页大小全局写入之后 |

三个 Apple 命名 helper 的实际函数体均为单条 `RET`，包括 `configure_timer_apple_regs`；不能只依据名字推断已配置 Apple timer，也没有必要单独为这条空调用安排真机轮次。

## 已确认的返回状态缺口

`wfe_timeout_configure` 调用既有启动参数解析器，按 timebase 计算 `arm64_eventi`。当前固定命令行没有 `wfe_events_sec`，源码默认每秒一百万个事件。当前 19.2 MHz 输入对应 19 ticks，所计算的 EVENTI 为 4；这只是该输入下的预期，需要由新增模型及执行结果核对。

实际 `wfe_timeout_init` 在 `0xfffffff0073f5dfc` 读取 `CNTKCTL_EL1`，在 `0xfffffff0073f5e08` 写回。其正常分支保留原值并 OR 入 EVENTI 和低位事件／访问标志。预期结果必须从当次入口快照计算，不能假定原值为零或只比较一个固定常量。

version7 验收时的 `boot/entry.S` 尚未覆盖 `CNTKCTL_EL1`；144 字节快照的分配大小不等于已经覆盖这个寄存器。version11 实现已补齐以下生命周期：

1. 在真实 XNU 入口前，从可信路径保存原始 `CNTKCTL_EL1`。
2. 新检查点捕获并严格核对预期事件参数与寄存器变化；新读数不能替代原有捕获、页表、AT 或缓存门。
3. 所有允许返回的正常／诊断路径都恢复原值，并在交接前按架构要求同步；不得从故障现场提供的指针取恢复数据。
4. 主机系统回归用不同非零初值验证完整恢复，检查最终生成指令，并独立保留旧版本语义。

version11 的实际生成指令与三组 TCG 系统执行已核对运行值、非零原值恢复和 Linux 入口桩独立回读。随后 SC7180 真机也取得完整 40 字段，三个寄存器运行值及按入口值恢复通过；该次入口值均为零，非零恢复仍只作为主机模型证据。

## 继续到拓扑／线程／VM 的依赖

源码中的 `ml_parse_cpu_topology` 消费 `/cpus`，使用 `state` 识别启动 CPU，以 `reg` 作为必需物理标识，其他多项缓存／寄存器范围属性有默认或缺省路径。现有受限树提供唯一 running CPU 和 4 字节 reg；`ml_readprop` 源码支持该宽度。不能据缺省属性虚构 Apple PIO 地址，也不能据单 CPU 模型推断其他核可启动。

该函数写拓扑全局、cluster 映射及 `TPIDR_EL0`／`TPIDRRO_EL0`。version11 已按实际生成码审计 chosen／stall 分支和新增可达函数，逐字节核对拓扑对象、未用数组槽、父帧与页大小 12，并独立检查两个线程标识寄存器及恢复；随后 version11 自身也取得完整 40 字段硬件记录。启动 CPU 的后续关联与页大小的后续消费者另行审计。

在拓扑之后，`arm_init` 继续设置 CPU 数据／栈及 CpuDataEntries，进入 `thread_bootstrap`、`cpu_bootstrap`、`rtclock_early_init`、启动回调、timer callout、CPU／processor 初始化，再调用 `arm_vm_init`。不能把这些调用都视为无副作用的直线代码。线程对象、当前线程寄存器、初始化回调、锁／分配器、计时器控制和内存拥有权分别需要核对。

`rtclock_early_init` 的源码注册 timebase 回调，回调计算时间转换全局；这和实际启用虚拟计时器、中断控制器、SoC 时钟驱动是不同阶段。完整 VM 还涉及新分配和页表修改，现有完整表池“逐字等于原模型”的验证门不能直接作为新 VM 阶段的成功判定。

## 下一阶段工作范围

已将可限定的 WFE、启动参数、页大小及单 CPU 拓扑解析合并为 version11，精确停止在 `0xfffffff0079b3d4c` 的下一条 `ml_get_boot_cpu_number` 调用前。完整契约和已通过的主机验证见 [WFE／拓扑检查点](xnu-topology-checkpoint.md)，尚不包含线程初始化或 VM。该版本完整 40 字段真机记录现已取得；后续对象、栈、回调和分配范围仍需审计，显示可观测性另按独立路线推进。

所有新内存操作继续受同次引导重新验证的自有范围限制，新增全局、活动帧、完整页表和系统寄存器均有独立预期及拒绝用例。此前硬件验收保持有效，完整 XNU／iPadOS 启动尚未完成。显示可观测性可按 [固件显示复用审计](display-reuse-audit.md) 独立验证，不与本轮拓扑执行混合。
