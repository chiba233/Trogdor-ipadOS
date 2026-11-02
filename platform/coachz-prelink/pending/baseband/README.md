# 基带桥接层

让 iPadOS 自己的蜂窝栈（ABM、BBUpdater、CommCenter，设置里的蜂窝网络和 eSIM）把这台机器的 SC7180 modem 当成苹果机型上走 PCIe 的高通基带来用。逆向结论在 [docs/plans/modem.md](../../../../docs/plans/modem.md) 的三轮调研里。只在 ADT 有 `/chosen/coachz-baseband` 时启动，打包参数是 `tools/xnu_screen.py --baseband`，不加启动参数。

## 文件

- `coachz_bb_qmux.h`：qmux1/qmux2 上的 QMUX 与 modem 的 QRTR 互转。普通服务只去掉或加回 6 字节 QMUX 头；CTL 在这里应答（GetClientId 按通道和服务分配客户端号，每个客户端占一个本地 QRTR 端口，从 0x4100 起；ReleaseClientId 和 Sync 释放，释放的端口要发 DEL_CLIENT）。苹果私有服务（0xe1 到 0xea）的请求交回调用者，暂时一律回 `QMI_ERR_NOT_SUPPORTED`。
- `coachz_bb_data.h`：这台 modem 的数据口和激活。CommCenter 绑定数据口时填苹果 PCIe 基带的端点 `{3, 4}` 或只给 mux id，这里换成或补上 IPA 的 `{4, 1}`（WDS 0xa2、QOS 0x2b、WDA 0x20/0x21）。CommCenter 第一次要 WDS 客户端时，桥接层用自己的客户端照 ModemManager 对 IPA 的顺序发 DPM Open Port 和 WDA Set Data Format（QMAPv4，不行再 QMAP）。BSP 的 ActivationRegister（0xb00c）回成功并发 kActivated 指示。
- `coachz_bb_sahara.h`：sahara 通道上扮演 modem 的设备端。发 HELLO，按 ELF 头、程序头、各段请求列出的镜像，收到的数据丢弃，最后一个镜像的 DONE 应答报完成。 主机回模式 3 时走命令模式（芯片 ID、序列号，再切回模式 0）。`pbl` 通道的方法 5 读 BHI 寄存器（0 序列号、3 MSM ID、4 PK hash），方法 4 `sendImage` 收下即丢。序列号是固定值，不用真实序列号。
- `coachz_bb_control.h`：`AppleBaseband` UserClient 的方法表。供电、复位、复用等只记录，不动 modem；管理器状态和基带状态原样存取；SPMI、GNSS、时间同步回 unsupported。
- `coachz_bb_channels.h`：通道名表、匹配用的类名和属性键、两个通道 UserClient 的方法号和参数个数。
- `coachz_baseband.cpp`：内核 IOKit。发布 `AppleBaseband`、`AppleBasebandPCIControl` 和`libPCITransport` 通道表里每个通道一个 `AppleBasebandPCIInterface`（属性 `ABPInterfaceProtocol`，`matchPropertyTable` 比较它：这个键 IOKit 自己不比较）。pbl、sahara、qmux1、qmux2 有实现，其余目前能打开、写入只计数、读不到数据，第一次打开时记一行日志，UserClient 用苹果的类名（`AppleBasebandUserClient`、`AppleBasebandPCIControlUserClient`、`AppleBasebandPCIUserClient`），沙盒按类名放行。 `AppleIPAppender`（`AppleIPAppenderUserClient`）和 `AppleBasebandPCIMAVPDPManager` 是数据面服务：ABM 的 `data::TransportService` 不检查空指针就用 `ApplePDPHelperInterface`，少了它们 CommCenter 就崩溃。`pdp_ip` 接上之前所有方法回成功和全 0。读写分同步和异步两种，异步按 libIOABP 的约定完成：引用数组第 1 项是回调，第 2 项是 refcon，完成时带一个参数，为字节数。BBUEUR13Loader 在 `pbl` 上读寄存器和 `SendImage`，然后打开 `sahara`，这时发 HELLO。调用方线程只排队；QRTR 收发都在 Wi-Fi 线程上。
- `patches/xnu/coachz-prelink-baseband.patch`：两处内核改动。
  - `bsd/dev/coachz_wifi.c`：GLINK 起来后调 `coachz_baseband_configure`，发布服务；本地端口 0x4100 起的 QRTR 数据交给桥；NEW_SERVER 和 DEL_SERVER 同步给桥；每毫秒调一次 `coachz_baseband_poll`，把排队的 QMI 请求和 DEL_CLIENT 发出去；周期报告里加一行桥的计数。`coachz_baseband_archive` 从磁盘读整个固件归档并核对 SHA-256。
  - `bsd/kern/kern_exec.c`：有 `/chosen/coachz-baseband` 时，禁用任务清单不再包含 CommCenter。launchd 改写过的清单里 CommCenter 还是 true 就不接受，按新清单重装。在 launchd 之前，把内核分区里的固件文件写到 `/private/var/coachz/baseband/Mav16`，内容不同才写。再用只读联合挂载把它显示在两处：`/private/preboot/<boot-manifest-hash>/usr/local/standalone/firmware/Baseband/Mav16`（ABM 的个性化固件路径），以及 `/usr/standalone/firmware/Baseband/Mav16`。SystemOS 叠在 `/` 上，`/private/preboot` 是 OS 卷的空目录，所以挂载检查只查挂载点本身和固件目录。
- `tools/xnu_screen.py --baseband`：
  - 设 `/chosen/coachz-baseband`。
  - 把根节点 `compatible` 换成 `J121AP`。`HWModelStr` 取自它，`libTelephonyUtilDynamic` 按它查基带表（`__TelephonyRadiosDetermineRadio`）：J120 不在表里，CommCenter 走 `NoBBRadioModule`；J121 是 radio 3、高通，固件正好是这份 IPSW 的 Mav16。`model`、`target-type`、`artwork-device-subtype` 不变，屏幕布局不受影响。
  - 嫁接 J121AP 的 `/baseband` 节点（compatible `baseband,n41`、device_type、class、`euicc-chip-id = 2`；GPIO 功能和中断是 J121AP 的控制器，不带）和 `/product baseband-chipset = mav16`。机型身份不变。
- `tools/linux_log.py --baseband`：从固定的 IPSW（`.tools/ipsw`）取出 Mav16 的 bbfw，把其中 17 个文件按 GPU 插件的布局打成魔数为 `CZBBFW01` 的归档（约 32 MiB），作为 `baseband.arc` 放进内核分区的固件块，排在 modem 固件之后。固件不加载它，所以签名容器不变大；内核分区用满 256 MiB。

主机测试：`tests/test_baseband_bridge.py`。

## 上机看什么

`com.apple.telephony.abm` 子系统的 `%s` 消息和 `com.apple.telephony.bb`（qmux 类别逐条打印 QMI 报文）的字节默认是 `<private>`。`bsd/kern/kern_exec.c` 在 `/private/var/preferences/Logging/Subsystems/` 下给这两个子系统写配置打开它们。这些日志可能带设备标识，只留在本机。

日志环里以 `COACHZ baseband:` 开头的行：
- 服务发布了几个；
- 固件文件写了几个、两处联合挂载到了哪一步（stage 5 才算成功）；
- 每 10 秒一行通道计数；
- 第一次出现的私有 QMI 消息；
- 不支持的方法号；
- sahara 的结果。

Data 卷的统一日志里看 ABM 和 BBUpdater：
- `FW base path` 和 `FW path ... exists`：ABM 用的是哪个固件目录；
- `Hello received boot mode`；
- `request image ID`；
- `item not found in item list`；
- `All images transferred`。

## 真机结果

BBUpdater 接受 SBL（镜像号 0x15）后报告启动成功，ABM engage，53 个 QMI 客户端接到 modem，`pbl`、`sahara`、`qmux1`、`qmux2` 都走自己的通道。modem 的工厂数据（FSG）放进固件块后，“关于本机”显示 IMEI 和 ICCID，SIM 就绪。激活指示带上服务已开通和两个卡槽已接受后，有效 SIM 在 LTE 上注册（漫游）并完成 CS、PS 附着，状态栏显示运营商名和信号格（目前要开机后插拔一次 SIM）；DPM、WDA 设置和数据口绑定都成功。蜂窝数据和 eSIM 还没有通过，卡点见 [docs/plans/modem.md](../../../../docs/plans/modem.md)。

## 没核实的

来自静态分析，真机上还没有单独验证：
- CTL 里除 GetClientId、ReleaseClientId、Sync、GetVersionInfo 以外的请求一律回成功。
- 异步读写的回调参数个数是从 libIOABP 的调用处推出来的。
- 时间事件从不发送；共享内存映射只回成功，不做实际映射。

## eSIM Tape 桥接

`coachz_bb_esim.h` 已补齐并接入 `coachz_baseband.cpp`，随现有 baseband 构建复制：独立 UIM 客户端发现实际 eUICC，Get Card Status 添加每槽能力 TLV 0x21，VINYL 的 Tape 请求经 ISD-R 发送，真实结果分片回到 CommCenter。空闲发现不切槽；只有显式 Tape 操作可临时切槽，结束关闭通道并恢复原卡槽。切换期间普通 UIM 请求排队，临时卡状态指示不冒充原逻辑 SIM；恢复未确认时不把原 SIM 请求转给另一张卡。

现有主机模型已覆盖 Tape/APDU 分段、能力字段、切槽恢复、续读和超时清理，ASan/UBSan 与 C/C++ 头文件检查通过；这不覆盖 IOKit 并发或真实卡片。尚未构建内核或实机验收，不能据此宣称 eSIM 菜单、下载、启停、删除或跨重启持久化已工作。下一次按 `COACHZ baseband: esim event` 的阶段读结果；日志不输出 EID、APDU 或 profile 内容。无需重编 GPU addon，也不改变镜像参数；现有 baseband 构建会带入这两个源码文件。详细状态与参考见 [modem 计划](../../../../docs/plans/modem.md#esim-tape-实现待实机)。
