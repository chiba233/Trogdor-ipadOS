# EC 合盖／平板模式／键盘底座状态

这是复用公共 Chrome EC 通道的状态驱动，由 `patches/xnu/coachz-prelink-switches.patch` 接入内核，`coachz-switches=1` 打开。真机上合盖、平板模式和底座状态随实际状态变化；本服务不触发动作。

## 已实现

- 查询 EC 命令版本，使用 MKBP_INFO v1 的 SUPPORTED／CURRENT，读取 EC 已去抖的状态。
- 区分 LidOpen、TabletMode、BaseAttached、FrontProximity，只解释 EC 声明支持的位，保留原始掩码。
- 有效时约每 200 ms 轮询；错误或无已知能力时退避到 5 秒。始终使用当前状态查询，不消费 MKBP 事件队列。
- 发布 `CoachzECPhysicalSwitches` IOService；以不可变 `SwitchState` 字典整体替换快照。
- 首次有效值、能力变化、错误后恢复建立基线；连续有效样本之间才记录真实的已知位变化。不会把读取失败当成闭盖／拔出。
- 基线、状态变化或可用性变化成功发布后，通过 `kIOMessageServicePropertyChange` 通知 general-interest 订阅者重新读取属性。

这个服务提供后续输入／电源策略所需的状态，**不自动触发锁屏、睡眠、背光变化、键盘禁用或按键注入**。它不是已经接通 iPadOS 合盖策略的声明。

## 源码与接入

- `coachz_switches.h`：可移植的命令客户端及变化跟踪逻辑。
- `CoachzSwitches.cpp`：工作循环、IOService、原子快照发布与变化通知。
- `coachz-prelink-switches.patch`：增加这两个文件、`iokit/conf/files` 登记和 sampler 启动钩子的独立补丁，未加入活动 `series`。
- [SOURCES.md](SOURCES.md)：固定源码、EC handler、底座 ADC／支架 Hall 的区别及位语义批注。

补丁在公共 EC／电池／输入／扬声器补丁之后应用，不需要改 ADT；没有该参数就不访问 EC。

## `SwitchState` 契约

| 字段 | 含义 |
| --- | --- |
| Status / Error | 当前有效性与错误；正数传输错误保持公共 EC 编码，负数是本地协议错误 |
| HostSequence / HostCompletionNS | 主机轮询序号和命令完成时 uptime 纳秒，不是物理开关的中断时间 |
| CommandVersions / SupportedMask | EC 回报的命令版本与开关能力 |
| RawState / KnownState | 仅有效时存在；原始状态，以及与 supported 和已知位掩码的交集 |
| ChangedMask | 仅有效时存在；与上次连续有效且能力相同的样本比较得出的变化位 |
| Baseline | 初次、能力变化或失败后恢复的快照，不应当作一次物理动作 |
| TransitionCount | 观察到已知状态变化的轮次数；同轮多位变化只计一次，不是 EC 事件总数 |
| LidOpen / TabletMode / BaseAttached / FrontProximity | 仅本轮有效且相应位被 supported 声明时存在的布尔值 |

缺失布尔值表示未知／不支持，不等于 false。`LidOpen=true` 表示开盖，不能照搬 Linux `SW_LID` 的反向语义。错误快照省略全部物理状态字段，防止旧值冒充本轮结果。64 位 OSNumber 的 Error 按 `int64_t` 解读。

未能分配或替换字典时，旧快照仍保留原来的时间和序号。变化通知不是可靠事件日志；轮询可能漏掉两个采样之间的短动作，通信中断期间发生了什么也不能恢复。消费者应以最新快照为准，检查状态、时间和 Baseline。

## 物理语义限制

Coachz 的 `KS_ATTACHED/KS_OPEN` 是 **kickstand 支架**，不是键盘底座。键盘底座由 `board/coachz/base_detect.c` 的 ADC 检测路径管理，本驱动查询它通过 EC 导出的 BaseAttached，而不是读支架 GPIO。FrontProximity 是 EC 报告值，可能来自 body-detection／支架启发式，不表示有独立人体距离传感器。

EC 底座内部有 UNKNOWN／DISCONNECTED／CONNECTED 三种状态，但 host 位只有布尔值；UNKNOWN 也可能导出 false。驱动无法从一个位恢复丢失的内部状态，因此初次快照仅建立基线，不能据此宣称刚发生了一次拔出。

## 检查与后续实机验收

```sh
python3 -B -m unittest discover -s tests -p test_switches.py -v
```

主机检查通过：六组查询／状态变化模型、可移植头文件 C++ 语法、候选补丁应用及源码一致性。IOKit 生命周期没有单独验证。

真机上底座连接／拆卸、合盖与平板模式的状态变化都进入日志，见 [硬件记录](../../../../docs/hardware-tests.md)。合盖熄屏走 iPadOS 原生的 Smart Cover 路径（`coachz-prelink-smart-cover.patch`）。
