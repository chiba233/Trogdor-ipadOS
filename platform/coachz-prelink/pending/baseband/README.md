# 基带桥接层

让 iPadOS 自己的蜂窝栈（ABM、BBUpdater、CommCenter，设置里的蜂窝网络和 eSIM）把这台机器的 SC7180 modem 当成苹果机型上走 PCIe 的高通基带来用。逆向结论在 [docs/plans/modem.md](../../../../docs/plans/modem.md) 的三轮调研里。只在 ADT 有 `/chosen/coachz-baseband` 时启动，打包参数是 `tools/xnu_screen.py --baseband`，不加启动参数。

## 文件

- `coachz_bb_qmux.h`：qmux1/qmux2 上的 QMUX 与 modem 的 QRTR 互转。普通服务只去掉或加回 6 字节 QMUX 头；CTL 在这里应答（GetClientId 按通道和服务分配客户端号，每个客户端占一个本地 QRTR 端口，从 0x4100 起；ReleaseClientId 和 Sync 释放，释放的端口要发 DEL_CLIENT）。苹果私有服务（0xe1 到 0xea）的请求交回调用者，暂时一律回 `QMI_ERR_NOT_SUPPORTED`。
- `coachz_bb_data.h`：这台 modem 的数据口和激活。CommCenter 绑定数据口时填苹果 PCIe 基带的端点 `{3, 4}` 或只给 mux id，这里换成或补上 IPA 的 `{4, 1}`（WDS 0xa2、QOS 0x2b、WDA 0x20/0x21）。CommCenter 第一次要 WDS 客户端时，桥接层用自己的客户端照 ModemManager 对 IPA 的顺序发 DPM Open Port 和 WDA Set Data Format（QMAPv4，不行再 QMAP）。BSP 的 ActivationRegister（0xb00c）回成功并发 kActivated 指示。
- `coachz_bb_sahara.h`：sahara 通道上扮演 modem 的设备端。发 HELLO，按 ELF 头、程序头、各段请求列出的镜像，收到的数据丢弃，最后一个镜像的 DONE 应答报完成。 主机回模式 3 时走命令模式（芯片 ID、序列号，再切回模式 0）。`pbl` 通道的方法 5 读 BHI 寄存器（0 序列号、3 MSM ID、4 PK hash），方法 4 `sendImage` 收下即丢。序列号是固定值，不用真实序列号。
- `coachz_bb_control.h`：`AppleBaseband` UserClient 的方法表。供电、复位、复用等只记录，不动 modem；管理器状态和基带状态原样存取；SPMI、GNSS、时间同步回 unsupported。
- `coachz_bb_channels.h`：通道名表、匹配用的类名和属性键、两个通道 UserClient 的方法号和参数个数。
- `coachz_baseband.cpp`：内核 IOKit。发布 `AppleBaseband`、`AppleBasebandPCIControl` 和`libPCITransport` 通道表里每个通道一个 `AppleBasebandPCIInterface`（属性 `ABPInterfaceProtocol`，`matchPropertyTable` 比较它：这个键 IOKit 自己不比较）。pbl、sahara、qmux1、qmux2 有实现，其余目前能打开、写入只计数、读不到数据，第一次打开时记一行日志，UserClient 用苹果的类名（`AppleBasebandUserClient`、`AppleBasebandPCIControlUserClient`、`AppleBasebandPCIUserClient`），沙盒按类名放行。 `AppleIPAppender`（`AppleIPAppenderUserClient`）和 `AppleBasebandPCIMAVPDPManager` 是数据面服务：ABM 的 `data::TransportService` 不检查空指针就用 `ApplePDPHelperInterface`，少了它们 CommCenter 就崩溃。`coachz_bb_pdp.cpp` 发布四个 raw IP `pdp_ip` 接口，按真实 WDS 状态和地址字段更新 IPv4/IPv6 上下文，经 IPA 的 QMAPv4 数据通道收发；尚未实现的方法仍返回占位结果。读写分同步和异步两种，异步按 libIOABP 的约定完成：引用数组第 1 项是回调，第 2 项是 refcon，完成时带一个参数，为字节数。BBUEUR13Loader 在 `pbl` 上读寄存器和 `SendImage`，然后打开 `sahara`，这时发 HELLO。调用方线程只排队；QRTR 收发都在 Wi-Fi 线程上。
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

BBUpdater 接受 SBL（镜像号 0x15）后报告启动成功，ABM engage，53 个 QMI 客户端接到 modem，`pbl`、`sahara`、`qmux1`、`qmux2` 都走自己的通道。modem 的工厂数据（FSG）放进固件块后，“关于本机”显示 IMEI 和 ICCID，SIM 就绪。激活指示带上服务已开通和两个卡槽已接受后，有效 SIM 在 LTE 上注册（漫游）并完成 CS、PS 附着，开机后状态栏直接显示运营商名和信号格（发给 ABM 的队列放 1024 帧：64 帧时开机必溢出，丢掉的应答会让 CommCenter 硬重启基带，或让 SIM 和注册状态停在旧值）；DPM、WDA 设置和数据口绑定都成功。运营商包的配置文件（BSP 0xa000 分段、SetConfig 0xe00d）照苹果基带的应答收下后丢弃，最后一段之后发 FileTransferStatus 0xa001，CommCenter 缓存配置后不再循环重启基带。eSIM 原生安装和启用已有实机结果；蜂窝 IPv4 数据已完成 PDP 重建后的网页加载，连接建立和 eSIM 注册期间仍有卡槽掉线，详见 [docs/plans/modem.md](../../../../docs/plans/modem.md)。

## 没核实的

来自静态分析，真机上还没有单独验证：
- CTL 里除 GetClientId、ReleaseClientId、Sync、GetVersionInfo 以外的请求一律回成功。
- 异步读写的回调参数个数是从 libIOABP 的调用处推出来的。
- 时间事件从不发送；共享内存映射只回成功，不做实际映射。

## eSIM Tape 桥接

`coachz_bb_esim.h` 已补齐并接入 `coachz_baseband.cpp`，随现有 baseband 构建复制：独立 UIM 客户端发现实际 eUICC，Get Card Status 应答及 Card Status Change 指示均添加每槽能力 TLV 0x21，VINYL 的 Tape 请求经 ISD-R 发送，真实结果分片回到 CommCenter。空闲发现不切槽；只有显式 Tape 操作可临时切槽，普通操作结束关闭通道并恢复原卡槽；下载会话按下述生命周期保持通道。切换期间普通 UIM 请求排队，临时卡状态指示不冒充原逻辑 SIM；恢复未确认时不把原 SIM 请求转给另一张卡。

实机已完成原生手动添加、卡片挑战、服务端认证、下载准备、配置包装载和启用命令。完整配置包进入桥接后按结构分段送入卡片，卡片返回 `9000`；原生重新列出一个配置，并报告 `Vinyl install operation succeeded`。蜂窝数据最终能够访问网页，但连接建立仍有掉线重试，eSIM 注册期间另一卡槽也会短暂离线；跨重启持久化和长期稳定性仍待验收。主机模型覆盖 Tape/APDU 分段、能力字段、切槽恢复、续读和超时清理，ASan/UBSan 与 C/C++ 头文件检查通过，不能替代 IOKit 并发或长期实机验证。阶段日志不输出 EID、APDU 或 profile 内容。协议修订只需重编内核，无需重编 GPU addon。

初始化兼容：原版 Vinyl 驱动启动时先用 `0x1000 / GetCsn` 查询两个槽，通用 `NOT_SUPPORTED (0x5e)` 不会完成 ready 回调。桥接返回明确的 `VINYL_NOCAPABLE_UICC (0x204)`：原版 `asVinylDriverError` 将它映射到 6，`handleClientStarted_sync` 的 CSN 回调明确接受此结果并发布对应槽的 Ready。这里没有 Apple legacy Vinyl CSN，GSMA EID 仍只由 Tape EID 请求读取；不伪造 CSN、不改 SIM Ready 或激活结论。卡状态指示与查询应答保留同一套能力字段，避免插拔后丢失 Tape 能力。这两处已有便携模型覆盖，实机初始化链已继续到真实 ES10 下载流程。

Tape 会话：EID/设备能力请求按原版 writer 从 TLV `0x01` 读取，设备能力查询允许不指定槽（原版填 0）。TapeInfo 结果头按原版 parser 的 11 字节布局返回：命令 1、状态 2、保留 2、事务号 4、错误字节 2；原来的 9 字节布局会使结果解析失败。`GetSessionData` 的空请求由桥接返回空恢复记录，`ClearSessionData` 只处理桥接的空记录集合，二者不转成卡片 APDU，不表示已支持跨重启恢复。挑战、服务端认证、准备下载沿用同一 VINYL 客户端的 ISD-R 通道，交互期间保留 180 秒空闲上限。装载完成后进入有界的通知批次，连续只读查询也可复用通道：空闲 3 秒或批次累计 15 秒后关闭并恢复原卡槽。启用、停用、删除、重置、取消、清理、错误或客户端断开仍触发清理。其它客户端不插入这一会话。状态日志仍不输出 APDU、EID 或 profile 内容。

## 原生 Internet 数据通路

固定系统版本的 CommCenter 把 Apple postponement ticket 缺失作为 Internet 上下文的前置阻塞条件。`tools/coachz_cellular_compat.py` 检查原始 SHA-256、Mach-O UUID 和 80 字节指令窗，只改一条条件分支，随后生成保留原有 entitlements 的 ad-hoc 签名副本和匹配的 v2 trust cache。WDS 激活、地址查询和网络返回值继续走原版逻辑。

准备和打包都需显式指定兼容副本。以下路径均相对于项目根目录，输入文件从对应系统版本的原始系统中取得。

```sh
python3 tools/coachz_cellular_compat.py --commcenter ORIGINAL_COMM_CENTER --output build/coachz-cellular-compat
# 在现有 xnu_screen.py 和 linux_log.py 打包命令中同时加入
# --baseband-commcenter build/coachz-cellular-compat/CommCenter
```

内核归档携带签名副本，ADT 携带其 SHA-256，信任缓存携带其 cdhash。启动时核对摘要，把副本安装到 Data，再用只读 bindfs 联合挂载到 CoreTelephony Support 目录。准备工具支持可选 `--compact`，需安装 `zopfli`，生成的流仍使用既有 zlib 解压路径。

实机已记录兼容副本挂载成功、WDS `0x20` 成功和 `0x2d` 返回地址字段。PDP 重建后，`pdp_ip0` 持续收发，发送计数从 1121 增至 2667，接收计数从 1743 增至 5759，驱动队列丢包为 0。统一日志记录了 112 条经该接口的有效 DNS 回复，WebKit 的蜂窝路径和 HTTP 200 响应均已出现，Safari 网页加载通过。APN 编辑入口、IPv6 数据连接、跨重启及长时间连接尚待验收。

两处生命周期修复支撑原生重连。eSIM 的 `ListNotification (BF28)` 与资料查询共用六项只读缓存，每项保留 16 KiB，改变卡片或通知状态的命令清空缓存，避免两项只读查询互相清缓存、重复切走实体卡。蜂窝接口注册 `PF_INET`／`PF_INET6` 的 protocol plumber，初始挂接和重连复用同一回调，保留 `EEXIST` 返回语义。此前 CommCenter 卸载旧 IPv4 协议后，`in_domifattach` 找不到重新挂接入口并返回 `ENXIO (6)`，DNS 发包无法进入驱动；修复后该错误消失。

GSI 轮询同时按硬件完成指针判断待处理事务。周期诊断保留发送请求数、队列、完成环和全局 IP／UDP 计数，便于区分栈内拒绝、DMA 积压和校验错误。日志仅记录计数和状态。

构建和发布仅包含源码、补丁和准备工具。原始系统文件、签名副本、信任缓存、镜像、工厂校准数据和原始日志保留在本地。


## 可选 eSIM 添加入口适配

`tools/coachz_cellular_compat.py --esim` 为固定版本兼容副本加入四处适配。原生流程在旧基带身份下报告没有本地开通能力，GSMA 添加流程位和实际安装路径也检查旧机型默认卡类型。Coachz 的真实卡由 Tape/APDU 桥接提供，因此兼容副本继续原生修改权限、Vinyl 可用性和设置阶段检查，为通过这些检查的结果加入 GSMA 流程位，并让安装请求使用真实 Tape 卡。四个位置分别核对固定指令窗，机型属性沿用现有配置；任一指令窗不符时不执行任何 eSIM 修改。

实机已进入原生扫描和手动输入页面，并完成真实卡片挑战、AuthenticateServer、PrepareDownload 和 LoadBoundProfilePackage。资格检查走过本地材料缺失的降级路径，原生安装同意、卡片认证和服务端交互继续执行；配置包写入后返回成功结果，原生重新列出配置并报告安装成功。

第四处适配仅把本地签名材料缺失的条件分支接到原生已有的软失败降级路径，此路径已有实机结果。有签名材料时继续原有请求，后续 URL、编码和远端资格响应的失败处理保持原样。真实 eUICC 认证、下载包验证和安装同意流程继续由原生实现处理。准备时将 `--esim` 加到已有兼容副本生成命令，打包仍显式指定生成的 CommCenter；副本使用对应的信任缓存和摘要。


Tape 的配置包请求有两层分段：TLV `0x10` 的 count/index 只描述当前结构部分，flag 才表示整个命令是否结束。桥接按同一事务保留接收状态，允许各部分从 index 0 重新开始；首段读取 BF36 声明总长并有界分配，最后一部分到齐后才执行原有的 SGP.22 结构切分及 STORE DATA。此前把首段视为完整包，会提前解析失败，并以 `INVALID_ARG` 拒绝后续部分。回归覆盖跨部分累积、部分内分片和结束前不执行，实机完整配置包装载成功。


## 卡槽占用与数据建立

切换到 eUICC 会触发真实卡片事件。按 [Hermes 的完整 eUICC 操作边界](https://chromium.googlesource.com/chromiumos/platform2/+/main/hermes/modem_qrtr.cc#976)，配置包后的通知处理不在每条命令之后归还、再借用卡槽；上述短批次兼顾查询合并与恢复期限。与临时切槽对应的 WDS 断线原因 `MMGDSI_CARD_EVENT` 见 [libqmi 的协议枚举](https://chromium.googlesource.com/external/gitlab.freedesktop.org/mobile-broadband/libqmi/+/55e51667274ff9b5f84f92ed305f76647df47f6a/src/libqmi-glib/qmi-enums-wds.h#485)。

临时借用或恢复尚未确认时，新的 WDS Start Network 返回 `DEVICE_IN_USE`，避免给被临时替换的逻辑 SIM 建立错误数据上下文。Stop Network、状态查询与真实断线继续处理，不伪造连接成功或隐藏卡片刷新。批次复用、截止清理和忙状态边界的基础回归已通过；减少切槽与重连次数仍待实机验收。打包使用无临时流程探针、保留必要兼容指令的签名副本和对应信任缓存。
