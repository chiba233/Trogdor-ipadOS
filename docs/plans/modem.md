# LTE 基带（蜂窝数据）

2026-11-02：已写好六块，放在 [pending/modem](../../platform/coachz-prelink/pending/modem/README.md)，由 drivers/RamBlock/coachz_modem.c 就地编译接入，真机已跑通到握手完成（见“真机结果”），rmnet_ipa0 也已在真机上打开（见“数据通路接入”）：IPA 与 modem 的 QMI 握手层；IPA 探测期配置（主线 `ipa_config` 全部步骤）；GSI setup（主线 `gsi_setup`）；端点编程和命令通道（`ipa_setup` 里的 `ipa_endpoint_setup`、命令通道启动、`ipa_mem_setup`、`ipa_table_setup`）；LAN RX 和默认路由（`ipa_setup` 的最后一段）；`ipa_modem_start` 和数据通路（rmnet_ipa0 与 rmnet 的 QMAP）。主线 `ipa_setup` 到 `ipa_qmi_setup` 之前已全部写完。寄存器值已和 Debian 上主线驱动配好后的真值对照。

## 路线（照主线 Linux）

MPSS 已经在跑（Wi-Fi 补丁：MBA/PIL、SMEM、SMP2P、GLINK、QRTR、rmtfs，IPA 时钟已投、SMP2P 的 IPA 时钟查询已应答）。主线里蜂窝数据走 IPA：`drivers/net/ipa`，LTE 机型 `qcom,gsi-loader = "modem"`（`sc7180-trogdor-lte-sku.dtsi`），之上是 `rmnet_ipa0` 和 rmnet；联网控制（UIM、DMS、NAS、WDS、WDA）在 Linux 上是 user space 的 libqmi/ModemManager。主线顺序：

1. **IPA 探测配置（已写）**：寄存器、内存布局和 `mem_offset`、中断、uC、端点、资源组、SMP2P。
2. modem 加载 GSI 固件后经 SMP2P 发 `ipa-setup-ready` → `ipa_setup`：**GSI、端点、命令通道、内存和路由/过滤表、LAN RX、默认路由（已写）**。
3. **QMI 握手（已写）**：AP 开 0x31/实例 1 服务、找 modem 的 0x31/实例 2，发 INIT_DRIVER（带 v4.2 内存布局），应答 DRIVER_INIT_COMPLETE 和 INDICATION_REGISTER，发 INIT_COMPLETE。
4. **`ipa_modem_start`（已写）**：modem 端点、`rmnet_ipa0`（QMAP）、rmnet。
5. QMI 控制：卡状态、上线、注册、WDS 拨号拿 IP、WDA 数据格式；再往上才是 iPadOS 的网络接口和蜂窝界面。

## 写了什么

IPA 配置：`gen_ipa_data.py` 在主机上编译主线的 `ipa_reg-v4.2.c`、`gsi_reg-v4.0.c`、`ipa_data-v4.2.c`，按主线枚举把真实的表导出成 `coachz_ipa_v42.h`（寄存器偏移、步长、字段掩码，端点表，资源限额），没有手抄的数字。`coachz_ipa_proto.h` 是可主机测试的判定：硬件寄存器值、`SHARED_MEM_SIZE` 解码与区域校验、canary 位置、FLAVOR_0 端点检查、资源限额编码、uC 应答规则。`coachz_ipa_config.c` 按主线顺序做 hardware → mem → interrupt → uc → endpoint → resource（v4.2 的 table 配置是空操作），另有中断处理（`ipa_isr_thread` 的主体，uC 报 INIT_COMPLETED 时置 `uc_loaded` 并经 AOSS QMP 发 `ipa_pc` 保持请求）、modem 启动前的 uC 供电标记、panic 时通知 uC。

GSI：`coachz_gsi_proto.h` 是主线 `gsi_channel_init` 建出的通道表（AP 通道按端点名顺序分事件环：命令 TX 通道 1 用环 0、LAN RX 通道 2 用环 1、modem TX 通道 0 用环 2、modem RX 通道 3 用环 3；v4.2 要 AP 替 modem 分配通道 0–3）、通道数据校验、上下文寄存器值、命令字、通用命令结果、环在一块 DMA 内存里的摆放。`coachz_gsi.c` 按 `gsi_setup` 做：查 GSI_STATUS、中断初始化、读通道和事件环数量、清错误日志、每个 AP 通道先分配并编程事件环再分配并编程通道，最后用通用命令替 modem 分配通道，失败时照主线回退。主线发命令后睡在 completion 上等 ISR，这里改为轮询同一个 ISR 主体，最长 50 ms。

命令通道：`coachz_ipa_cmd_proto.h` 是可主机测试的部分：`ipa_endpoint_program` 对每个 AP 端点写的寄存器值、`gsi_trans.c` 的 TRE 和事件格式、`ipa_cmd.c` 全部立即命令的载荷、`ipa_cmd_init` 的检查、`ipa_table_init` 建的表区，以及 `ipa_mem_setup`、`ipa_table_setup` 的命令清单（v4.2 没有哈希表：4 条表初始化加 2 条过滤表清零）。`coachz_ipa_cmd.c` 依次做端点编程、启动命令通道（通道 1）、发命令事务：每条命令占一个 TRE，载荷放在按 TRE 环下标对应的 DMA 槽里，最后一个 TRE 带 IEOT 和 BEI；完成判定照 `gsi_channel_update`，读事件环的 `EV_CH_E_CNTXT_4`，用事件里的 TRE 指针对上最后一个 TRE，再敲事件环门铃。主线等完成没有超时，这里给 1 秒并报告超时。`coachz_ipa_kernel.h` 放三个内核文件共用的结构。

LAN RX：`coachz_ipa_rx_proto.h` 是可主机测试的部分：缓冲和 TRE 布局（每个 8 KiB，数据放在 NET_SKB_PAD 之后）、最多挂多少（`gsi_channel_tre_max`，这里是 251）、补充时的门铃规则（每 16 个一次，或者 TRE 用完时）、包状态结构和 `ipa_endpoint_status_parse` 的跳过/标签/丢弃/上交规则、默认路由值。`coachz_ipa_rx.c` 做 RX 端点启用（启动通道、打开该端点的挂起中断、补充缓冲）、轮询完成（从事件取收到的长度、解析状态、缓冲重新挂回、确认事件、重新打开 IEOB），以及 `ipa_setup` 的收尾：LAN RX 启用后写 ROUTE。主线里 LAN RX 没有网络设备，上交的包只计数丢弃；命令端点发来的带标签状态表示管道清空完成。

数据通路：`coachz_ipa_modem_proto.h` 是可主机测试的部分。数据格式由 IPA 端点决定：AP_MODEM_TX 开了校验和、头长 8，AP_MODEM_RX 开了校验和，对应 rmnet 的 MAPv4（拆聚合 + 上下行 v4 校验和，flags 0x0d）。上行每个包前加 QMAP 头和全零的 UL 校验和头（即 rmnet 的软件校验和路径）；下行拆聚合、去掉 QMAP 头和 8 字节 DL 校验和尾（相当于 rmnet 没开 RXCSUM，由协议栈自己校验）。rmnet 数据设备 MTU 是 1500 − 8 = 1492。`coachz_ipa_modem.c` 做 `ipa_modem_start`、打开 rmnet_ipa0（启动 AP_MODEM_TX 的通道 0，AP_MODEM_RX 经 `coachz_ipa_rx` 挂缓冲）、按 mux id 建数据设备、发送（每帧拷进 DMA 中转槽、一个 TRE，主线在分片过多时也会线性化成一段）、轮询 TX 完成，以及按 mux id 上交 IP 包的下行回调。AP_MODEM_RX 没开状态元素，`coachz_ipa_rx.c` 按主线 `ipa_endpoint_skb_build` 把整个聚合缓冲交给 rmnet。还没有 XNU 的网络接口；已打开设备的停止（通道 stop/reset）也没移植。

握手：`coachz_ipa_qmi.h`：QMI 帧格式、IPA QMI 的消息号和 TLV（照 `ipa_qmi_msg.c` 的编码表）、按 v4.2 内存布局和运行时 `mem_offset` 构造 INIT_DRIVER、解析应答、服务端应答和 INIT_COMPLETE 指示，以及 `ipa_qmi_ready` 的就绪规则（首次启动要等指示发出；modem 重启时保留 uC 和首启状态）。

主机测试 `test_ipa_qmi.py` 9 项通过：数据通路模型 3 组（数据格式与端点一致、MTU、TX TRE，上行封帧，含 IPv4、带填充的 IPv6、MAP 命令和截断尾部的聚合缓冲拆帧）、RX 模型 3 组（缓冲 TRE、251 个缓冲的门铃规则、每条状态解析规则、默认路由与 Debian 的 ROUTE 0x01100050 一致）、命令模型 4 组（端点 1、6、8、9 的十个端点寄存器都和 Debian 真值一致，TRE 和事件，各条命令载荷逐字段，内存和表 setup 的命令清单，含 Debian 的 LOCAL_PKT_PROC_CNTXT 0x5f0）、GSI 模型 4 组（四个 AP 通道的通道和事件环上下文、QOS、scratch、HW_PARAM_2、通用命令结果都和 Debian 真值一致）、握手模型 3 组、配置模型 4 组（含实机的 SHARED_MEM_SIZE 0x415 和 Debian 的 FLAVOR_0），`coachz_ipa_config.c`、`coachz_gsi.c`、`coachz_ipa_cmd.c`、`coachz_ipa_rx.c`、`coachz_ipa_modem.c` 用 XNU 桩头文件做类型检查（后三个还按 b9 的方式合成一个编译单元检查）、生成头标明 Linux 提交、现有构建不引用它们。

## 交给接手者

配置：Wi-Fi 补丁的 `wifi_ipa_power()` 之后调 `coachz_ipa_config(&ipa, zero_virt, zero_phys, wifi_qmp_send)`（8 KiB 设备可见内存；apps SMMU 处于旁路），主线是在 modem 启动之前的探测阶段做；MBA/PIL 开始启动 modem 时调 `coachz_ipa_modem_starting()` 并清掉 AP 的 SMP2P `ipa` 项（主线 `ipa_smp2p_notify_reset`）；GIC SPI 311 或轮询里调 `coachz_ipa_interrupt()`。另外 Wi-Fi 补丁打印 `ipa: setup ready` 时把 SHARED_MEM_SIZE 的高低 16 位读反了：主线 MEM_SIZE 在 15:0，MEM_BADDR 在 31:16，`mem_offset` = 8 × 高 16 位。

GSI：配置时一并调 `coachz_gsi_init(&gsi, ipa.gsi, ring_virt, ring_phys, bytes)`，给一块清零、64 KiB 对齐、写合并或非缓存、至少 40 KiB 的内存（主线也在 probe 时分配环）；收到 `ipa-setup-ready` 后，`ipa_setup` 第一步调 `coachz_gsi_setup()`。再调 `coachz_ipa_cmd_init(&cmd, &ipa, &gsi, virt, phys, bytes)`（至少 8448 字节、清零、128 字节对齐的 DMA 内存，64 KiB 环内存块从 0xc000 起还有空间）和 `coachz_ipa_setup_commands(&cmd)`。然后 `coachz_ipa_rx_init(&lan, &ipa, &gsi, CIPA_EP_AP_LAN_RX, virt, phys, bytes)`（128 字节对齐的写合并或非缓存内存；2 MiB 能挂满 251 个缓冲，和 Linux 一样，给得少就挂得少）和 `coachz_ipa_setup_lan_rx(&lan)`，`ipa_setup` 到此结束。`coachz_gsi_interrupt()` 在 `ieob_pending` 里标出 LAN RX 的事件环时，或者定时，调 `coachz_ipa_rx_poll(&lan)`。之后是 `ipa_qmi_setup`（握手），再往后是 `ipa_modem_start` 和 `rmnet_ipa0` 数据通路，还没写。

握手：在现有 QRTR 上：开 0x31 实例字 0x101 的服务；modem 通告 0x31/0x201 时发 INIT_DRIVER（需要第 1 步读到的真实 `mem_offset`）；收到 0x20、0x35 请求时应答；每个事件后跑 `ci_ready_step()`。这一层要等第 2 步 `ipa_setup` 做完才有意义。

## 已有的实机事实

XNU 下（还没接这些代码）：modem 发了 `ipa-setup-ready`（SMP2P `ipa` 项 0x2），当时 AP 还没做任何 IPA 配置；SHARED_MEM_SIZE 是 0x415，`mem_offset` 为 0，报告的 8360 字节超过 ipa-shared，按主线 `mem_size` 仍取 0x2000；QRTR 上有 0x31 版本 1 实例字 0x201 的服务。Debian 上（主线驱动 setup 完成，`rmnet_ipa0` 存在）把 IPA 设为常开，经 /dev/mem 只读读了寄存器：FLAVOR_0 0x08090811、HW_PARAM_2 0xe4046b59（11 个通道和事件环）、资源限额、COUNTER_CFG、IDLE_INDICATION 、全部 GSI 通道和事件环上下文、四个 AP 端点的十个端点寄存器、LOCAL_PKT_PROC_CNTXT（0x5f0，说明那边 `mem_offset` 也是 0）、ROUTE（0x01100050）、IRQ_SUSPEND_EN（0x100，`rmnet_ipa0` 没开时只有 LAN RX）都和这里写的一致。CLKON_CFG（0）、QSB_MAX_WRITES（0x78）、QSB_MAX_READS（0x7800000c）和 TX_CFG 的 PA_MASK_EN 与主线 probe 时写的不同；读之前设备处于运行时挂起，主线恢复时不重写它们，所以视为挂起后的值，代码保留主线 probe 的写法。

## 真机结果（XNU，一次启动，日志环）

b9 用 drivers/RamBlock/coachz_modem.c 按上面的顺序接入，在 Wi-Fi 线程的 1 ms 循环里轮询，没有接 SPI 311/432。预期日志全部一致：SHARED_MEM_SIZE 0x415（mem_offset 0，mem_size 0x2000）、FLAVOR_0 0x8090811、HW_PARAM_2 0xe4046b59、四个 AP 通道和事件环都已分配、modem 通道 0xf、两次命令事务完成（LOCAL_PKT_PROC_CNTXT 0x5f0，ENDP_INIT_MODE[6] 0x83）、LAN RX 挂了 251 个缓冲、ROUTE 0x1100050、IRQ_SUSPEND_EN 0x100；配置后 CLKON 读到 0x30000000、QSB 0x8/0xc，就是主线写的值。没有超时、状态错误、generic result 或 global error。握手走到了 INIT_COMPLETE：INIT_DRIVER（mem_offset 0，uc_loaded 0）得到成功回答，uC 报 INIT_COMPLETED 并发出保持请求，INDICATION_REGISTER 和 DRIVER_INIT_COMPLETE 都已应答。日志记录的 240 秒内状态一直保持，modem 在跑，Wi-Fi 不受影响；LAN RX 没收到包。

对照主线：INIT_COMPLETE 之后 modem 发来的 0x23 和 0x27 请求，在 `ipa_server_msg_handlers` 里没有处理函数（只有 0x20 和 0x35），而 `qmi_invoke_handler`（drivers/soc/qcom/qmi_interface.c）对没有处理函数的消息直接返回、不回应，所以只记日志不应答就是主线的行为。第一次 INIT_DRIVER 里 uc_loaded 为 0 是预期的：modem 首次启动时负责加载 uC，只有 modem 之后重启时 `skip_uc_load` 才为 1。INIT_DRIVER 回答里的 `modem_driver_init_pending` 为 1，表示还有第二次握手（DRIVER_INIT_COMPLETE），它也确实到了；主线解码回答，但只用其中的结果字段。

## 数据通路接入

握手完成（`ci_ready_step` 要求启动 modem）后：`coachz_ipa_rx_init(&modem_rx, &ipa, &gsi, CIPA_EP_AP_MODEM_RX, virt, phys, 2 MiB)`、`coachz_ipa_modem_init(&m, &ipa, &gsi, &modem_rx, tx_virt, tx_phys, bytes)`（每个 TX 槽 2 KiB，1 MiB 正好是通道上限 505 个），再 `coachz_ipa_modem_start(&m)`、`coachz_ipa_modem_open(&m)`。事件环 3 调 `coachz_ipa_rx_poll(&modem_rx)`，事件环 2 调 `coachz_ipa_modem_tx_poll(&m)`。没有数据呼叫也先打开，这和 Linux 拉起 rmnet_ipa0 时一样；要等 QMI 控制（WDA 数据格式、WDS 绑定 mux id、拨号）建好呼叫才会有数据，这部分还没写。预期日志："rmnet_ipa0 up: TX channel 0 (505 slots), RX endpoint 9 with 251 buffers, IRQ_SUSPEND_EN 0x300"。

真机已通过：rmnet_ipa0 两行日志符合预期，打开后 GSI 通道 0 和 3 的状态都是 2（已启动），没有数据呼叫时 modem RX 计数保持 0。还没有走过任何数据。

## iPadOS 蜂窝界面和 eSIM（调研，2026-11-02）

验收标准：蜂窝要在 iPadOS 界面里显示和配置（状态栏信号和运营商，设置里的蜂窝网络、APN、SIM 和 eSIM），并且要接 eSIM。只做内核拨号加一个裸网卡不算完成。

先例：UniXNU 的移植文档只有 Wi-Fi；ios-sweet、UniXNU/sheng、Droid-VM 都禁用 CommCenter，没有人接过蜂窝界面。

以下来自本地 21H461 IPSW 的内核缓存（prelink 信息）和共享缓存里库的名字、字符串，没有反汇编验证。

**苹果的栈就是“高通 modem 走 PCIe”。** 内核缓存里有：`AppleBasebandN71`（板级，供电和复位，按 `baseband,n41` 匹配，`AppleBasebandUserClient`）、`AppleBasebandPCI`（匹配 `baseband-pcie` 的 PCIe 设备，MHI/BHI）、`AppleBasebandPCIMAVControl`（控制面，`AppleBasebandPCIControlUserClient`，按 mav16 到 mav30 各代配置）、`AppleBasebandPCIMAVPDP`（数据面，`AppleBasebandPCIPDPQMAPSkywalk` 等在 Skywalk 上建 `pdp_ip`）。它的 MHI 通道表是高通标准的：2/3 SAHARA、4/5 DIAG、8/9 QDSS、10/11 远程 EFS（refs）、14/15 和 16/17 两路 QMUX、32/33 AT、100/101 QMAP 数据、109/110 QMAP 控制、105/106 低时延、104 RSC、92/93 语音。我们的 modem 是同一类芯片，只是控制走片上 QRTR、数据走 IPA，没有 PCIe/MHI。

**系统服务。** ABM（`libBasebandManager`）经 `libTelephonyBasebandDynamic`（`TelephonyBaseband*`：上电、复位、状态，对应 `AppleBaseband` 的 UserClient）和 `libPCITransport`（`TelephonyBasebandPCITransport*`：按编号打开管道读写、时间同步、共享内存映射，对应 `AppleBasebandPCIControl` 的 UserClient）接内核；在 QMUX 管道上建 QMI 客户端，再经 XPC 把 QMI 服务交给 CommCenter（`QMITransportService`、`QMuxServerAccepter`）。ABM 还自带远程文件系统（`RFS/FSModule`、`BBUFSServer`，相当于 rmtfs）和固件刷写（`BBUpdater`，经 SAHARA）。另有 `libmav_ipc_router_dynamic`（高通 IPC Router，即 QRTR），用途待查。

**QMI 服务。** 标准：CTL、WDS、DMS、NAS、QOS、WMS、UIM、PDC、WDA、DSD、DFS、SSCTL 等。苹果私有（只在苹果定制固件里有，我们的固件没有）：BSP（Board Support Package，ABM 用得最多）、AWD、VINYL、MFSE、MAVIMS、ELQM、Mav P2P、BSP APPS、Stewie。

**eSIM。** VINYL 的 “Tape” 操作（读 EID、eUICC 挑战值和信息、列出 profile、装载绑定 profile 包、启用/停用/删除 profile）就是 SGP.22 的 ES10 函数。标准做法是经 QMI UIM 打开到 ISD-R 的逻辑通道发 APDU（Linux 上 lpac、ModemManager 这么做），所以 VINYL 可以由桥接层用 UIM 实现。机器上 eUICC 的卡槽和 EID 还没读：要在 Debian 上用 QMI UIM 读卡槽状态。

**路线。** 内核里扮演苹果的基带接口：发布 `AppleBaseband` 和 `AppleBasebandPCIControl` 的 UserClient，让 ABM 以为有一个已经启动好的 PCIe 高通 modem；QMUX 管道转到 QRTR 上的 QMI（QMUX 的服务类型和客户端号对 QRTR 的服务和端口，CTL 由桥自己实现，libqmi 的 QRTR 后端就是这么做的）；EFS 继续由内核的 rmtfs 服务；数据面把 IPA 的 rmnet 接成 `pdp_ip`；苹果私有服务由桥应答，VINYL 用 UIM APDU 实现；启用 CommCenter。机型身份不换（换成别的屏幕尺寸，屏幕比例会坏）：保持 iPad14,1/J310，只把 IPSW 里蜂窝版 J121AP 设备树的 `/baseband`（`compatible = baseband,n41`、`euicc-chip-id`、GPIO 功能和 `config`）、`/arm-io/apcie/.../baseband-pcie` 和 `/product` 的 `baseband-chipset = mav16` 嫁接进来，并在 MobileGestalt 缓存里补蜂窝能力；确实需要蜂窝型号时用同屏幕的 iPad14,2/J311。已经真机通过的 IPA、GSI、握手、rmnet_ipa0 都还用得上。

**还要查（按顺序）：** ABM 启动基带的状态机，怎样跳过 SAHARA 刷写（字符串里有 “Transport bypass”“skipping establishing transport”）；两个 UserClient 的方法号和参数；CommCenter 启动和注册时必需哪些 BSP、AWD 等私有消息，哪些可以回“不支持”；`pdp_ip` 由谁建、CommCenter 怎么配置它；在不换机型身份的前提下，要嫁接哪些 ADT 节点、补哪些 MobileGestalt 项；Debian 上读 eUICC 的卡槽状态和 EID。

### 第二轮调研（反汇编 ABM、IOABP、TelephonyBaseband，2026-11-02）

都来自 21H461 的 IPSW：用 Xcode 自带的 dsc_extractor 从共享缓存解出库，用 otool 反汇编（库里带符号），内核 kext 信息来自内核缓存的 prelink 信息。苹果二进制只留在本机临时目录，不进仓库。

**启动。** `BootModule::boot_sync` 先问 `capabilities::abs::shouldBoot()`，返回 0 才走“Faking bootup”；这份正式版里它写死为 1，所以 ABM 每次都真启动：`BootControllerLegacy::prepare` 检查 `/usr/standalone/firmware/Baseband/` 里有固件，`bootModem` 交给 BBUpdater。正常开机（非恢复模式）不做个性化（“Not in restore mode - skipping gathering Personalization parameters”），只是把恢复时写好的镜像经 SAHARA 推给基带，等它进入运行态；还支持 BypassManifestStatusCheck、BypassNVMDownload/Sync 等选项。所以桥接层要在 sahara 通道上当一个接收端，收下镜像丢弃，然后报告运行态；磁盘上要放好固件文件。IPSW 里自带 `Firmware/vinyl_02_Mav16-11.00.01.Release.bbfw`（33 MB，含 SBL1、RPM、TZ、MBA、QDSP6SW、APPS、ACDB、BBCFG 和 eUICC 签名 profile），镜像打包时解到该目录即可，这些镜像永远不会真的在我们的 modem 上运行。

**设备树。** IPSW 里有蜂窝版 J121AP、J208AP 的设备树。J121AP 有 `/device-tree/baseband`（`compatible = baseband,n41`，`AppleBasebandN71` 按它匹配；`euicc-chip-id = 2`；bb_on、bb_rst、radio_on、coredump、reset_det 等 GPIO 功能；`config`）、`/arm-io/apcie/pci-bridge3/baseband-pcie`，`/product` 有 `baseband-chipset = mav16`。机型身份不换（iPad14,1/J310），只嫁接这几处。

**AppleBaseband UserClient（`libTelephonyBasebandDynamic` → `__TelephonyBasebandControllerCallMethod`）方法号：** 0 复位，1 上电/断电，2 复用开关，3 读复位，6 断电重启，8 VBUS，11 读 PMU EXTON，16 读 DIAG 就绪，17 读 EFS 擦除完成，18 读复位请求，19 coredump GPIO，20 时间同步，22/23 设置/读取管理器状态，25 充电器去敏，26/27 读写基带状态，28/29 读写基带属性，32/34 频点表，35–37/41/45 SPMI 和 GNSS，42 coredump 就绪，43 经 SPMI 让基带崩溃。都是 GPIO 和状态，内核里直接应答即可。

**通道 UserClient（`libIOABP`）。** 每条通道一个 `AppleBasebandPCIInterface` 服务，`IOABPClass::start_nl` 按名字匹配（名字应是 MHI 通道表的协议名：qmux1、sahara、refs、diag……）。方法号：0 open（一个标量），1 close，2 write，3 read（各有同步 `IOConnectCallMethod` 和异步 `IOConnectCallAsyncScalarMethod` 版本），4 sendImage，5 readRegister，6 中止通道，7 启动通道，8 时间同步，9/10 映射/解除映射共享内存，11/12 注册/注销时间事件。控制连接 `AppleBasebandPCIControl`（`IOABPControlClass`）：0 open，1 close，2 读日志，3 日志器调参；日志另有 `AppleBasebandPCITraceBackendRaw`。上层 `libPCITransport` 只是包装这些。

**私有 QMI。** CommCenter 的高通命令驱动里能看到 BSP（ActivationRegister、SetAntennaPreference、EnableAQMReport、GetCDRXInfo、SetAudioLoggingEnabled……）、AWD（指标上报）、ELQM、P2P、VINYL（Info、TapeInfo、VSimEnable、EosStatus）、AUDIO（语音，iPad 用不上）。静态看不出哪些是注册的硬依赖：桥接层对没实现的私有消息先回“不支持”，上机读 CommCenter 和 ABM 的统一日志再补。

**数据面。** 高通这条路的 `pdp_ip%1u` 由内核 `AppleBasebandPCIMAVPDP` 在 QMAP 上建（`pdp_ip0`…），CommCenter 拨号后配置它；`ApplePDPHelper` 属于另一条数据面（ACDP/ACIPC），这里用不到。QMAP mux id 来自 CommCenter 发给基带的 WDS 绑定 mux 请求，桥接层能直接看到，按它把 IPA rmnet 的 mux 和 `pdp_ip` 对上。

**MobileGestalt。** 蜂窝能力主要从 IORegistry（`AppleBaseband` 服务、`/baseband` 节点）推出；身份不变，让 MobileGestalt 在有基带节点的 ADT 上重建缓存（`coachz-reset-mgcache` 已有）。

**桥接层的组成（据此）：** 内核里发布 `AppleBaseband`（上面的方法表，状态由我们的 MBA/PIL 和 SMP2P 驱动）、`AppleBasebandPCIControl`、各通道的 `AppleBasebandPCIInterface`（qmux1/qmux2 ↔ QRTR 上的 QMI，自带 CTL；sahara 当接收端；refs 由已有 rmtfs 处理或回空；diag/at 等先不开）、`pdp_ip` 网卡接 IPA rmnet；私有服务和 VINYL（UIM APDU）由桥应答。

**还没查的：** `AppleBasebandPCIInterface` 用什么属性区分通道（名字放在哪个键里）；ABM 读基带状态的取值含义（26/27 号方法的状态码）；BBUpdater 在 sahara 上实际收发的报文（hello、读芯片信息、image transfer、done），决定接收端要回什么；QMUX 帧格式和 CTL 客户端号分配（可照 libqmi 的 QRTR 后端）；`pdp_ip` 是 Skywalk netif，普通 ifnet 能否被 CommCenter 接受。

### 第三轮调研（通道匹配、状态、SAHARA、QMUX，2026-11-02）

**通道服务怎么被找到。** `IOABPClass::start_nl` 用 `IOServiceMatching("AppleBasebandPCIInterface")`，再往字典里放 `ABPInterfaceProtocol = <名字>`（ASCII 的 CFString），等首次匹配和终止通知。名字来自 `libPCITransport` 的 `{编号, 名字}` 表：0 pbl、1 sahara、2 diag、3 refs、4 qmux1、5 qmux2、6 rom、7 boot、8 ari、9 trace、10 at、11 dpl、12 mediactl、13 vcom、14 gnss、15 qdss、17 virtual_timesync、20 boot_logger。所以内核里每条通道发布一个类名为 `AppleBasebandPCIInterface` 的服务，属性 `ABPInterfaceProtocol` 写通道名。打开：`IOServiceOpen` 类型 0，然后方法 0 带一个标量 0；服务上的 `IOGeneralInterest` 消息转给 EL0（错误和状态通知，消息码还没查）。

**通道方法的参数（IOConnectCall* 前的寄存器）。** 2 write、3 read：同步版两个标量加输入结构；异步版两个标量（EL0 缓冲地址、长度），引用数组 3 项，第 1 项是 EL0 回调、第 2 项是 refcon，IOKit 按参数个数调用回调，所以完成时回一个参数（字节数）。4 sendImage 两个标量，异步完成回两个参数。5 readRegister 一个标量。6 中止、7 启动通道各一个标量（方向），完成不带参数。8 时间同步三个标量，回一个参数。9 映射共享内存三个标量（区域号、地址、长度），10 解除映射一个标量。11/12 时间事件各一个标量。控制服务 `AppleBasebandPCIControl`：0 open（一个标量），1 close，2 读日志（异步，三个标量），3 日志器调参（四个标量）。

**基带状态。** `AppleBaseband` 的 26/27 号（读写基带状态）和 22/23 号（管理器状态）只是 ABM 自己写、自己读回：`BootController::setBasebandState` 先问 `capabilities::radio::supportsBasebandStateController()`（无线电类型在 9 到 15 之间才为真），再调 26/27 号；读回失败只记日志。内核不需要理解这些值，存住、原样返回即可。`AppleBaseband` 由 `libTelephonyBaseband` 按服务名 `AppleBaseband` 找。

**SAHARA。** `libETLSAH` 的报文号：1 HELLO、2 HELLO 应答、3 READ_DATA、4 镜像传完、5 DONE、6 DONE 应答、7/8 复位、9 内存调试、0xb 命令就绪、0xc 切换模式；苹果扩展：0xa0/0xa1 擦除查询和应答、0xa2 根清单查询、0xa3/0xa4 根清单应答、0xa5 哈希查询、0xa9 配置查询。模式：0 待传、1 完成、2 内存调试、3 命令、0x100 Maverick（苹果的清单和随机数流程）。Mav16 用 `BBUEUR16UpdateSource` 和 `BBUEUR10Programmer`：主机先等设备的 HELLO（应答里的保留字放产品类型和硬件配置），模式 0 或 1 进入镜像传输，按设备的 READ_DATA（镜像号、偏移、长度）发数据，镜像传完后发 DONE，设备回 DONE 应答（0 还有下一个，1 全部完成）。也就是说主机是被动的：发什么镜像、发多少完全由设备的请求决定。镜像号按 `EUR10Item` 的表：0x15 SBL1、0x17、0x19、0x1c、0x1d、0x1e、0x1f、0x06、0x08、0x05、0x10/0x11/0x14（EFS）、0x21、0x22、0x24、0x25、0x26、0x28、0x29、0x2a、0x30，以及苹果自己的 0x4300、0x4700、0x4c00、0x4f56、0x5400。桥接层在 sahara 通道上扮演设备：发 HELLO，按 ELF 头请求一个镜像的各段（像高通的 PBL 那样），收下丢弃，报告传完，DONE 应答报完成。要请求哪些镜像号、主机是否接受只请求一部分，由 BBUpdater 自己的日志（“request image ID”“item not found”“All images transferred”）在真机上确认。

**QMUX。** 苹果的 `QMuxHeader` 前 6 字节就是标准 QMUX 头：I/F 类型 1、长度（不含 I/F 字节）、控制标志（主机发 0x00，服务发 0x80）、服务类型、客户端号，后面是 QMI SDU。普通服务的 SDU 头（标志、2 字节事务号、消息号、长度）和 QRTR 上 QMI 的头完全一样，所以转发只是去掉或加上这 6 字节；CTL 的事务号是 1 字节，由桥接层自己应答。苹果的解析库里 CTL 只有 GetClientId（0x22）、ReleaseClientId（0x23）、Sync（0x27，含指示）、ConfigPowerSaveMode，以及 RevokeClientId 指示。QMUX 服务类型号：标准服务和 QRTR 服务号相同（WDS 1、DMS 2、NAS 3、UIM 11、WDA 0x1a、COEX 0x22、PDC 0x24、DSD 0x2a、SSCTL 0x2b、DFS 0x30 等）；苹果私有服务 AUDIO 0xe1、BSP 0xe2、CIQ 0xe3、AWD 0xe4、VINYL 0xe5、MAVIMS 0xe6、ELQM 0xe7、P2P 0xe8、APPS 0xe9、SFT 0xea，modem 上没有，由桥接层分配客户端号并逐条应答。每个 QMUX 客户端在 QRTR 上占一个本地端口（libqmi 的 QRTR 后端也是每个客户端一个套接字），释放时发 DEL_CLIENT。

## 下一步

桥接层第 1 到 3 项已写完，放在 [pending/baseband](../../platform/coachz-prelink/pending/baseband/README.md)，集成步骤也在那里：
1. 可移植部分：QMUX 与 QRTR 互转和 CTL、SAHARA 设备端、`AppleBaseband` 方法表。
2. 内核 IOKit 服务和 UserClient（`coachz_baseband.cpp`）。
3. Wi-Fi 线程的 QRTR 挂钩，以及 kern_exec.c 的固件写入、联合挂载和放行 CommCenter（`coachz-prelink-baseband.patch`）。

打包是 `tools/xnu_screen.py --baseband`（设 `/chosen/coachz-baseband`、嫁接 J121AP 的 `/baseband` 节点和 `baseband-chipset`）加 `tools/linux_log.py --baseband`（把 Mav16 固件放到内核分区里 modem 固件之后）。launchd 按 `/baseband` 选 `com.apple.CommCenter.plist`（`_LimitLoadToDeviceTree`），没有这个节点时用 `-noncellular` 那份。

真机上服务、固件和两处挂载都通过。`compatible` 用 J121AP 后 CommCenter 走高通分支并启动 ABM，状态栏显示搜信号。发布 `AppleIPAppender` 和 `AppleBasebandPCIMAVPDPManager` 后 CommCenter 不再崩溃，“蜂窝网络”页面能打开，固件目录 bind 挂载到 ABM 要的路径后 BBUpdater 能加载全部镜像，开始走 BHI：`boot` 通道、读寄存器（方法 5）、`SendImage`（方法 4）。之后才轮到 sahara 要请求哪些镜像、固件目录用哪一处、哪些私有消息必须实现，然后接 `pdp_ip` 和 VINYL（eSIM）。
