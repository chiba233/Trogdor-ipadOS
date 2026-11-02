# SC7180 温度采集适配

已接入：`patches/xnu/coachz-prelink-thermal.patch`，`coachz-thermal=1` 打开；源码和主机夹具在 [pending/thermal](../../platform/coachz-prelink/pending/thermal/README.md)。真机 25 路全部读到；`thermalmonitord` 通过 38 个 HID 温度服务取数，不再报找不到服务。

## 选择

已有电池、背光、键盘等路径正在推进，下一项选择依赖较少的芯片温度读取。相比直接实现动态调频或 EC 传感器系统集成，TSENS 只需核对两组 MMIO 布局和有效位，且可复用公开 UniXNU 的采样思路；其输出也能成为后续温控的输入。

先保留固件状态做只读采样，而非同时初始化控制器与调整频率。若固件没启用传感器，会明确报告不可用。

## 已编写

- 两组 bank、共 25 路，原始寄存器与逐通道状态，有效温度换算为 m°C。
- 版本、控制器、reset、sensor enable、TRDY、VALID 的检查，有限重读，每轮清除旧结果。
- 有效通道峰值和约 10 秒周期日志；初始报告保留各路值与失败状态。
- 独立候选补丁和 ADT 生成函数。

移植中发现固定版本 NativeQcomTsens 的 ready 检查使用 bit 3，Linux 的 SC7180 对应 TSENS v2 定义为 bit 0。新代码按后者实现，并有专门模型检查；真机 25 路全部读到。

## 主机验证

四项检查通过：独立 C 模型（六组关键行为）、实际装载器 Node 的 ADT 序列化、候选补丁在隔离夹具中应用并与源码一致、构建接线。真机读数见 [硬件记录](../hardware-tests.md)。

## 还没做的

TSENS 初始化／校准、阈值中断、DVFS、温控策略、PMIC NTC、iPadOS thermal 服务和休眠恢复。当前日志中的 bank peak 不直接代表完整 CPU 温度；限频只有内核 80 °C 规则，`thermalmonitord` 没有 CLPC／PMGR 后端。

