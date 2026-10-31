# EC 开关状态源码证据与批注

日期：2026-11-01。EC 固定为 `coreboot/chrome-ec@ad41bf945927e186eaad768a9d51f45ada67b9b9`。本轮直接核对 handler 和板级底座实现，不凭头文件或 GPIO 名称猜状态。新客户端／服务按 BSD-2-Clause 编写；Linux 只用于语义比对，未复制其 GPL 实现。

## 1. 查询接口确实不会取走事件

[common/mkbp_info.c](https://github.com/coreboot/chrome-ec/blob/ad41bf945927e186eaad768a9d51f45ada67b9b9/common/mkbp_info.c)，blob `f4753c78bd6e226258ca1955179b7502f23c8d78`。

```text
EC_CMD_MKBP_INFO = 0x0061; declared command versions 0 and 1
request: uint8 info_type, uint8 event_type
info_type SUPPORTED=1, CURRENT=2; event_type SWITCH=4
SUPPORTED/SWITCH: get_supported_switches(), response_size=4
CURRENT/SWITCH:   mkbp_get_switch_state(), response_size=4
```

批注：本客户端先查询 `GET_CMD_VERSIONS`，明确选支持的 v1，再发送 `[1,4]` 和 `[2,4]`。两次返回都是 32 位小端位图。没有 GET_NEXT_EVENT、FIFO_READ 或开关 setter；它与按钮队列消费者的职责分开。

同一文件的 `get_supported_switches()` 根据 CONFIG_LID_SWITCH／TABLET_MODE_SWITCH／BASE_ATTACHED_SWITCH／FRONT_PROXIMITY_SWITCH 生成能力掩码。能力不是所有机器恒为 `0xf` 的常量；代码每轮查询，不把不存在的位当成 false。

## 2. 当前值来自 EC 已去抖的状态

[common/mkbp_input_devices.c](https://github.com/coreboot/chrome-ec/blob/ad41bf945927e186eaad768a9d51f45ada67b9b9/common/mkbp_input_devices.c)，blob `38d58af2c6f4ae0e3a44ea96d5fefb64ca3a5c36`。

该文件 `mkbp_get_switch_state()` 的来源对应关系：

```text
bit 0 LID_OPEN:         lid_is_open()
bit 1 TABLET_MODE:      tablet_get_mode()
bit 2 BASE_ATTACHED:    base_get_state()
bit 3 FRONT_PROXIMITY:  body_detect_get_state(), when BODY_DETECTION_NOTIFY_MKBP is enabled
```

批注：函数说明这些 getter 读取已去抖状态，无副作用或额外延迟。AP 不需要重新驱动 ADC／Hall GPIO 或再实现一套电平去抖。能力声明和取值的配置条件并非完全相同，尤其 FrontProximity 不能凭 supported 位就认定有独立距离传感器；实际固件配置和物理语义仍须实机核对。

[ec_commands.h 的位定义](https://github.com/coreboot/chrome-ec/blob/ad41bf945927e186eaad768a9d51f45ada67b9b9/include/ec_commands.h#L4218-L4237)，blob `b237a84b19870060a2ee2113c3c854780b3b7999`，固定四个位号。原始位图中其他位保留为 RawState，不生成已知动作。

## 3. 真正的键盘底座：ADC 路径

[board/coachz/base_detect.c](https://github.com/coreboot/chrome-ec/blob/ad41bf945927e186eaad768a9d51f45ada67b9b9/board/coachz/base_detect.c)，blob `fbae1698dec9018ca1a1c2b64f51bc5b42548fd9`。

```text
base_status: UNKNOWN=0, DISCONNECTED=1, CONNECTED=2
ADC connected range: 120..300 mV
ADC definitely disconnected: >=1500 mV
attach debounce: 350 ms; detach debounce: 20 ms
unclear ADC range: retry after 500 ms
```

底座状态改变后，EC 的关键操作为：

```c
int connected = (status == BASE_CONNECTED);
gpio_set_level(GPIO_EN_BASE, connected);
tablet_set_mode(!connected, TABLET_TRIGGER_BASE);
base_set_state(connected);
```

批注：这些是 EC 固件已经负责的检测／供电／状态操作，新 AP 驱动不重复执行。UNKNOWN 和 DISCONNECTED 都可能通过布尔 host 位导出 false；协议没有提供内部三态，因此初始化与恢复快照只作为基线，不生成拔出事件。

## 4. 支架 Hall 不能当作键盘底座

[board/coachz/board.c 的 ks 初始化与处理](https://github.com/coreboot/chrome-ec/blob/ad41bf945927e186eaad768a9d51f45ada67b9b9/board/coachz/board.c#L148-L200)，blob `c1113d4f682760830c6a1a708742df671701df8e`。

```text
ks_init: Kickstand switch initialization
ks_attached = !GPIO_KS_ATTACHED_L
ks_open = GPIO_KS_OPEN
proximity_detected = !(ks_attached && ks_open)
body_detect_change_state(proximity_detected, false)
```

批注：KS 是 kickstand。这条路径的物理动作和用途不同于上面的键盘 base ADC。此前调研若把 KS 两路称为键盘底座 attached/open，应以上述明确分离的源码为准。本驱动没有把这些 GPIO 解释为 BaseAttached。

## 5. 合盖方向与上层策略

[Linux cros_ec_keyb.c](https://github.com/torvalds/linux/blob/36aad21d50d1af3f6954bb0c988257a908202477/drivers/input/keyboard/cros_ec_keyb.c#L131-L141)，blob `0a817680171c991f1132d706f8c68af54ccd4528`。

其映射把 `EC_MKBP_LID_OPEN` 对应 Linux `SW_LID`，并显式设置 `inverted=true`；TabletMode 则不反转。新服务保留协议的 LidOpen 名称，true 代表开盖。Linux 的 input 代码不是可直接用于 iPadOS 的策略接口。

本轮先搜索现有 UniXNU／Sheng／ios-sweet 本地文档和驱动，没有找到可直接复用的 Coachz EC 开关前端。本实现只发布 IOService 状态及标准 property-change 通知。XNU `IOService::messageClients()` 向 general-interest 订阅者派发消息，`IOWorkLoop::runAction()` 用于停止时串行化定时器回调；相关 API 已对照当前 XNU 头文件／实现核对，未编译该内核对象。

没有向 IOPMrootDomain 发送 clamshell-close、没有调用睡眠／电源接口；状态可读不代表 iPadOS 已根据它自动熄屏或切换输入策略。
