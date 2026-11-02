# EC 合盖、平板模式和底座状态

已接入：`patches/xnu/coachz-prelink-switches.patch`，`coachz-switches=1` 打开；源码和主机夹具在 [pending/switches](../../platform/coachz-prelink/pending/switches/README.md)。真机上合盖、平板模式和底座状态随实际状态变化；本服务不触发动作。合盖熄屏走 iPadOS 原生的 Smart Cover 路径（`coachz-prelink-smart-cover.patch` 加 MobileGestalt 能力项），当前版本没带缓存，所以合盖只关背光。

## 选择与实现

这块依赖少，但对后续键盘拆装、显示和电源策略重要：沿用公共 EC，用 MKBP_INFO 的 SUPPORTED／CURRENT 读取去抖后的合盖、平板模式、底座与接近状态。发布 IOService 原子快照，真实变化或可用性变化时通知订阅者；错误、初始化和恢复都不伪造闭盖／拔出动作。

不另写 ADC／Hall 驱动，不取走共享事件队列，也不提前触发尚未验证的深睡。源码核实还区分了支架和键盘底座：KS_ATTACHED／KS_OPEN 属于 kickstand，键盘 base 则有独立 ADC 检测实现，不能混用。

## 证据

主机：查询与变化跟踪 C 模型六组行为、可移植头文件 C++ 语法、候选补丁应用及源码比对；motion 和 switches 两种顺序组合应用均通过。真机：底座连接／拆卸、合盖与平板模式的状态变化进入日志。

