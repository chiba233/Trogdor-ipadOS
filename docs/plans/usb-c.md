# USB-C（usb_1：DWC3、QUSB2、QMP）

2026-11-02：第一阶段 SoC bring-up 已写好，放在待接入目录，尚未编译进 XNU，也没有实机结果。入口：[实现与接入说明](../../platform/coachz-prelink/pending/usb/README.md)，[固定源码](../../platform/coachz-prelink/pending/usb/SOURCES.md)。

## 硬件和路线

两个 USB-C 口都在板载 Realtek RTS5411 集线器后面，集线器挂在唯一的 DWC3 控制器 usb_1 上（2.x 那半在根口 1，3.x 那半在根口 2）。Type-C 口由 EC 管（`cros-ec-typec`，两口在 AP 侧都是 host），QMP combo PHY 的 DP 一侧没接。照主线 Linux 7.4-rc1 里这块板用的驱动移植：`dwc3-qcom-legacy`、`dwc3` core、`phy-qcom-qusb2`、`phy-qcom-qmp-combo`、`gcc-sc7180`、`onboard_usb_dev`，数值和顺序取自主线，代码自己写；QMP 的 89 项表由脚本从固定提交的源码编译生成，不手抄。UniXNU 社区目录里的 NativeDwc3 面向小米平板 5（SM8150），没有真机结果，没有采用。

根盘已经在 eMMC 上，所以重新初始化 usb_1 不会拿掉根盘。

## 第一阶段做什么

`coachz-usb-soc=1` 时，`coachz_usb_soc_init()` 按主线顺序：集线器供电 GPIO84、USB30 GDSC、CX nom、主时钟（150 MHz 要求按主线规则落在 200 MHz）和 mock_utmi 19.2 MHz、glue 复位与 5 个时钟、dwc3 识别和端口数、PHY 前设置、QUSB2 初始化（含设备树调参和熔丝 trim）、QMP USB3 初始化、SUSPHY、软复位、GCTL、事件缓冲、GUCTL1/2、切到 host。之后 xHCI 交给现有 libpayload 栈（键盘、U 盘）或以后的驱动。

没做：唤醒中断和睡眠恢复、带宽投票、EC 的 Type-C 状态、DP。

## 主机验证

`python3 -B -m unittest discover -s tests -p test_usb_soc.py -v`，4 项通过：可移植层 7 组模型（ASan/UBSan）；`coachz_usb_soc.c` 对桩头文件 `-Wall -Wextra -Werror` 类型检查；生成表的来源和结构，首项与主线源码一致；当前构建没有引用。没有编译内核或内核对象，没有打包、写盘或访问设备。

## 交给接手者

1. 共用 RPMh 客户端加两样：CX 使用者 `COACHZ_CX_USB = 4`（总数改成 5）；按使用者聚合的稳压器请求 `coachz_rpmh_vreg_request`。ldoa4 同时给 DSI PLL、CSI、UFS、refgen 供电，各驱动直接写 VRM 会互相覆盖；eMMC 驱动的直接写也该改成走它。
2. 编进 `libCoachzUsb.a`，在 `coachz_usb_open()` 开头、`xhci_init()` 之前调用；返回 0 时不要继续 `xhci_init()`。
3. 核对事件缓冲页 `0x81130000` 和 io_submap 余量。
4. 候选镜像加 `coachz-usb-soc=1`，上机看 `COACHZ usb` 日志；验收是键盘和两个 USB-C 口上的 U 盘照常枚举，U 盘仍走 SuperSpeed。

## 下一阶段（要等实机结果和接手者的评估）

xHCI 以上用哪套栈：kernelcache 里 Apple 原版的 `AppleUSBXHCI`/IOUSBHostFamily 接到这颗 DWC3 上，还是别的 xHCI 驱动；然后是两个口的热插拔、EC 的 Type-C 状态和 PD、唤醒中断、带宽投票。
