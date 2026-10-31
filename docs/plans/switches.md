# EC 合盖、平板模式和底座状态

2026-11-01：新增待接入状态驱动，[源码与接入说明](../../platform/coachz-prelink/pending/switches/README.md)，[固定版本证据与批注](../../platform/coachz-prelink/pending/switches/SOURCES.md)。没有加入活动内核编译或进行实机验收。

## 选择与实现

这块依赖少，但对后续键盘拆装、显示和电源策略重要：沿用公共 EC，用 MKBP_INFO 的 SUPPORTED／CURRENT 读取去抖后的合盖、平板模式、底座与接近状态。发布 IOService 原子快照，真实变化或可用性变化时通知订阅者；错误、初始化和恢复都不伪造闭盖／拔出动作。

不另写 ADC／Hall 驱动，不取走共享事件队列，也不提前触发尚未验证的深睡。本轮源码核实还区分了支架和键盘底座：KS_ATTACHED／KS_OPEN 属于 kickstand，键盘 base 则有独立 ADC 检测实现，不能混用。

## 验证与交接

四项主机检查通过：查询与变化跟踪 C 模型（六组行为）、可移植头文件的 C++ 语法、候选补丁应用及源码比对、活动构建未启用。另在当前 XNU 两个上下文文件的临时副本中，将 motion 和 switches 按两种顺序组合应用，全部通过且新文件逐字节一致。

IOKit 服务代码只做源码/API 核对，未编译 XNU 对象。代码、候选补丁与文档留在待接入目录，未打包、写盘、访问设备或提交。

接手者接入并开启 `coachz-switches=1` 后，先取得真实 supported/current，分别测试底座连接／拆卸、合盖与平板模式，并比较 Debian 状态及 EC 并发访问。只有这些实机状态确认后，才进一步接到 iPadOS 输入、背光或电源策略；当前没有自动合盖睡眠或键盘禁用功能。
