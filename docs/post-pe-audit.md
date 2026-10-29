# 完整 PE 返回之后的初步审计

更新：2026-10-30。当前真机验收已到 version7 的完整 `PE_init_platform(FALSE)` 返回，全部 22 个严格字段通过并交接 Linux。这里记录下一段的源码／实际生成码观察和待实现条件；不是新检查点的运行结果，也没有新测试镜像。

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

目前 `boot/entry.S` 保存并恢复翻译、栈、线程、FP 访问及四个 EL1 异常寄存器，但没有 `CNTKCTL_EL1`。144 字节快照的分配大小不等于已经覆盖这个寄存器。推进到 WFE 之后必须先补齐：

1. 在真实 XNU 入口前，从可信路径保存原始 `CNTKCTL_EL1`。
2. 新检查点捕获并严格核对预期事件参数与寄存器变化；新读数不能替代原有捕获、页表、AT 或缓存门。
3. 所有允许返回的正常／诊断路径都恢复原值，并在交接前按架构要求同步；不得从故障现场提供的指针取恢复数据。
4. 主机系统回归用不同非零初值验证完整恢复，检查最终生成指令，并独立保留旧版本语义。

目前没有运行这段 WFE 代码，也没有把当前包装器描述为支持这项恢复。

## 继续到拓扑／线程／VM 的依赖

源码中的 `ml_parse_cpu_topology` 消费 `/cpus`，使用 `state` 识别启动 CPU，以 `reg` 作为必需物理标识，其他多项缓存／寄存器范围属性有默认或缺省路径。现有受限树提供唯一 running CPU 和 4 字节 reg；`ml_readprop` 源码支持该宽度。不能据缺省属性虚构 Apple PIO 地址，也不能据单 CPU 模型推断其他核可启动。

该函数写拓扑全局、cluster 映射及 `TPIDR_EL0`／`TPIDRRO_EL0`。后两者目前已在快照中，但拓扑对象、启动 CPU 关联、父子帧、chosen／stall 分支、页大小全局和新增所有可达函数仍需按实际生成码逐项审计。当前 PE 通过不能替代这些新数据模型。固定构建的页大小写入为 12；仍需核对对应全局和后续消费者。

在拓扑之后，`arm_init` 继续设置 CPU 数据／栈及 CpuDataEntries，进入 `thread_bootstrap`、`cpu_bootstrap`、`rtclock_early_init`、启动回调、timer callout、CPU／processor 初始化，再调用 `arm_vm_init`。不能把这些调用都视为无副作用的直线代码。线程对象、当前线程寄存器、初始化回调、锁／分配器、计时器控制和内存拥有权分别需要核对。

`rtclock_early_init` 的源码注册 timebase 回调，回调计算时间转换全局；这和实际启用虚拟计时器、中断控制器、SoC 时钟驱动是不同阶段。完整 VM 还涉及新分配和页表修改，现有完整表池“逐字等于原模型”的验证门不能直接作为新 VM 阶段的成功判定。

## 下一阶段工作范围

先补齐 WFE 的寄存器生命周期和完整源码／生成码调用图，再以 CPU 拓扑解析返回作为候选边界，把可限定的启动参数、页大小及单 CPU 拓扑工作合并验证。是否纳入线程初始化取决于后续审计能否限定对象、栈、可达分支和分配范围；尚未确定最终补丁 PC、版本格式或验收字段，不据此生成介质。

新增契约须保持全部内存操作位于同次引导重新验证的自有范围内，对所有新全局、活动帧、页表与系统寄存器提供独立预期、拒绝用例和可恢复路径；之后才做真实 CPU 模型执行、独立审查和新的外置介质准备。此前硬件验收保持有效，完整 XNU／iPadOS 启动尚未完成。
