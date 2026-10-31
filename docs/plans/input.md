# 机身按键与键盘

依据是 `Trogdor-Input-Research` 调研的顺序：机身按键 → 基本键盘 → 触控板光标与点击 → 滚动与多指 → 拔插与休眠。这里完成了前两步的代码。机身按键已接入构建，真机结果在 `docs/hardware-tests.md`；USB 键盘（`drivers/RamBlock/coachz_usb_hid.c`）已接入，由 `coachz-usb-keyboard=1` 打开。主机测试 `tests/test_input.py` 通过。

## 补丁和文件

| 位置 | 内容 | 启用方式 |
|---|---|---|
| `patches/xnu/coachz-prelink-input.patch` | `iokit/Kernel/CoachzInput.cpp`：在 `IOHIDResource` 下、触摸数字化仪旁边发布 ShengHidInput 的键盘/Consumer 设备（`native_hid_input_allocate`，报告 1 为 boot 键盘，报告 2 的第 1～3 位为 Power、音量加、音量减），并为它加一个内核 IOHIDEventDriver personality（与 CoachzHid.cpp 给数字化仪的做法相同）；每 20 ms 用 `EC_CMD_MKBP_INFO` 读 EC 按键的**当前状态**，变化时发报告 2；导出 `coachz_input_keyboard_report()` 给 USB 键盘发报告 1 | `series` 中放在电池补丁之后；`coachz-buttons=1`（只有音量）或 `=3`（加上电源键） |
| `drivers/RamBlock/coachz_usb_hid.c` | Zed 底座键盘（USB `18d1:504c` 的接口 0，boot keyboard）：`usb_hid_init()` 切到 boot protocol、SET_IDLE 0、开一个 interrupt-IN 队列；`coachz_usb_hid_poll()` 取报告转给上面的函数；`coachz_usb_enumerate_hubs()` 开机时只轮询 hub 一秒 | 见同目录 `README.md` 的五步接线 |

`input.patch` 与 `speaker.patch` 互不依赖，都以 `battery.patch` 为前提；电池→扬声器→输入、电池→输入→扬声器两种顺序都已验证能打上。

## 设计要点

- **报告怎么进系统**：直接调用该设备的 `handleReport`，也就是原版 `IOHIDDevice` 虚表第 181 项。`build-sheng-hid-input.py` 在每次构建时都核对第 181 项是原版的 handleReport（`0xfffffff004add0c4`），所以不需要改 `CoachzHid.cpp`、HID 构建脚本，也不需要它的空 `native_buttons_set_sink`。
- **不抢 EC 事件**：按键读的是 EC 的当前状态，而不是 `GET_NEXT_EVENT` 队列，因此不会吃掉电池或其他读者需要的 MKBP 事件。`ec_commands.h` 写明 `EC_MKBP_INFO_CURRENT` 支持 `EC_MKBP_EVENT_BUTTON`；固定版本源码里没有 EC 侧的处理函数，所以先查 `EC_MKBP_INFO_SUPPORTED`，若 EC 拒绝会打印并停止。
- **电源键默认不上报**：按电源键会让 iPadOS 进入睡眠，而睡眠尚未支持。EC 自己的长按强制关机不受影响。
- **键盘不会碰根盘**：`usb_poll()` 会对根盘做 TEST UNIT READY，失败就把它当作拔出；USB 键盘代码从不调用它，开机时的 hub 轮询也跳过根盘，之后只读键盘自己的中断队列，并且和块 I/O 用同一把锁。
- **只做 boot 协议**：默认是 report protocol，固定 8 字节只在 SET_PROTOCOL(boot) 之后才成立。顶排 Vivaldi 功能键、语言布局和输入法留给后续；驱动发的是按键 usage，不是字符。

## 真机上要看什么

日志进外接盘末尾的日志环，回到 Debian 后用 `sudo -n /usr/local/sbin/coachz-read-log` 读。

1. `COACHZ input: keyboard/consumer device: class 1 resource 1 personality 1 attached 1 started 1`。
2. `COACHZ input: EC buttons supported 0x7 (error 0x0), enabled volume`（电源、音量加、音量减为第 0～2 位）。
3. 按音量键时出现 `COACHZ input: buttons 0x2 -> report 0x4: 0x0`、松开时回到 `0x0`；屏幕上出现音量指示。
4. 接入 USB 键盘后：`COACHZ USB HID: keyboard at N, vendor 18d1 product 504c ... protocol 0 idle 0 queue 1`，在文本框里能打字，按键松开后不会连发。
5. 若 `EC button state error`：错误码的含义同电池补丁（`docs/plans/battery-speaker.md`）。

## 还没做的

- 触控板：Zed 的接口 2，多点 HID（subclass/protocol 均为 0，不是 boot mouse）。先看原版 HID 是否识别原始报告，否则先做基础指针和点击；不能套用触摸屏的绝对坐标和 edge-tip 处理。
- 顶排功能键、底座拔插、休眠恢复，以及电源短按/长按/唤醒的分别验收。
