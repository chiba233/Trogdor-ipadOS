# 屏幕背光

代码在 `platform/coachz-prelink/package/CoachzBacklight.cpp`、`CoachzBacklightStart.cpp` 和 `scripts/build-coachz-backlight.py`，由 `coachz-backlight=1` 打开。真机结果记录在 `docs/hardware-tests.md`。

## 做了什么

| 部分 | 内容 |
|---|---|
| 系统前端 | `CoachzBacklight` 继承原版 AppleARMPlatform 的 `AppleARMBacklight`，挂在原版 Legacy framebuffer 下，在 launchd 之前 `registerService()`。CoreBrightness 用 `IOObjectConformsTo(service, "AppleARMBacklight")` 认内置屏背光，只有这样才会启动 libEDR 服务。UniXNU-sheng 记录过：只发布 `backlight-control` 而类不符合时，QuartzCore 会持显示锁同步等待，屏幕停止更新。结构和发布的属性参考 unixnu-sm-t733 的 `NativeT733Backlight`，它在同一份 21H461 内核缓存上已由使用者确认亮度可调。 |
| 属性 | `IODisplayParameters`（`brightness` 0–255、`BrightnessMilliNits`）、`backlight-calibration-parameters`、`backlight-marketing-table`、`new-backlight-architecture`、`backlight-control`。`setProperties` 接受 `brightness-nits`（16.16 nits）、`brightness`（0–255）和可选的 `FadePeriod`（16.16 秒，最多 5 秒）。 |
| 后端 | Chrome EC `EC_CMD_PWM_SET_DUTY`（0x25，类型 2 DISPLAY_LIGHT，索引 0，duty 0–65535），经电池补丁的 `coachz_ec_command()` 发送。EC 总线只有这一个拥有者。 |
| 调度 | `setProperties` 不碰 EC，只记下最新请求并唤醒工作线程。工作线程每 20 ms 最多发一次 SET；新请求会替换尚未取走的旧请求。值稳定后用 `EC_CMD_PWM_GET_DUTY`（0x26）读回一次并写日志。 |
| 范围 | 最低 duty 是 3136（4.8%，即 Linux Trogdor 曲线的第一档 196/4096，DTS 注释说面板不喜欢低于约 5%）。duty 0 从不发送，所以最暗也不会熄屏。亮度与 duty 成线性。20/100/400 nits 是名义范围，没有在本面板上测过；到达硬件的只有 duty。 |
| 不碰的 | TLMM GPIO12（AP 背光使能）和 EC 自己的使能门保持开机探针设置的状态。没有 S3/S0、熄屏和睡眠联动。 |

## ABI 核对

`build-coachz-backlight.py` 在每次构建时用参考内核（`COACHZ_DISPLAY_REFERENCE`）核对以下各项，任何一项不符就停止：

- AppleARMPlatform `__TEXT_EXEC` 哈希 `6af4faa2…`。
- 元类：`0xfffffff006ade638`，类名 "AppleARMBacklight"，父类 `IOService::gMetaClass`，大小 0x370。
- 受保护构造函数 `0xfffffff0049abc1c` 的完整字节：调用原生 `IOService` 构造函数，写入 vptr `0xfffffff005ccbd18`。
- 析构函数 `0xfffffff0049abc84` 直接跳到原生 `IOService` 析构函数。
- 169 项具体虚表（无纯虚项）。原版只覆盖 IOService 的第 0、1、7、14、21、44、81、82、86、111、117、132、155 项。

我们的子类恰好覆盖这 13 项，每个都转回 `IOService`。这样原版 AppleARMBacklight 的任何方法都不会在这个对象（原版私有字段全为零）上运行。唯一指向原版代码的是它自己的第 168 项。这些地址正是 T733 脚本里的地址减去 0x1000000，即它的 `REFERENCE_SLIDE`。

## 主机验证

- `python3 -m unittest tests.test_backlight`：6 项通过。
  - duty/nits/档位映射：单调、不低于下限、可逆。
  - 16.16 转换和渐变插值。
  - 只发 PWM 两条命令，类型 2、索引 0。
  - `setProperties` 不等 EC。
  - 原版 13 个覆盖全部替换。
  - `CoachzDisplay.patch` 能打上。
  - 用本机参考内核核对 ABI；没有参考内核时自动跳过。
- 在已组装的内核树上用 `IOPlatformExpert` 的编译参数（`-Wall -Wextra -Werror`）完整跑了 `build-coachz-backlight.py`：
  - 编译、虚表核对、原版绑定和延迟类注册都通过，生成了 `libCoachzBacklight.a`。
  - 打补丁后的 `CoachzDisplay.cpp` 同样能编译。
  - 运行时 PE 尚未链接进内核。

## 真机上要看什么

日志进外接盘末尾的日志环，回到 Debian 后用 `sudo -n /usr/local/sbin/coachz-read-log` 读。

1. `COACHZ backlight: initial duty 52428 (error 0x0), mode 1`：开机探针留下的是 80%。
2. `COACHZ backlight: service conforms 1 attached 1 started 1, mode 1`。
3. 进入系统后，控制中心的亮度滑块能改变屏幕亮度。每次停下出现一行 `COACHZ backlight: target … applied … readback … (error 0x0)`；readback 与 applied 只差量化误差（NPCX 的计数分辨率），不要求逐位相等。
4. 显示和触摸不受影响：主屏仍在刷新，动画不卡住。卡住通常说明 CoreBrightness 认服务的路径出了问题，先用 `coachz-backlight=2` 排除 EC。
5. `coachz-backlight=2`：不发布 `backlight-control`，CoreBrightness 不接管。开机后屏幕会暗到 25% 约一秒再恢复，日志有两行 readback，用来单独验证 EC 通路。
6. 若出现 `set duty … failed: error 0x…`，错误码的含义同电池补丁（`docs/plans/battery-speaker.md`）。

## 还没做的

- 熄屏和睡眠：framebuffer 关闭时关背光（GPIO12 与 EC 门），唤醒后恢复之前的亮度。
- 本面板的真实 nits 标定和曲线手感调整。
- 自动亮度：环境光传感器链路没有调查。
