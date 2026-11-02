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

配置：Wi-Fi 补丁的 `wifi_ipa_power()` 之后调 `coachz_ipa_config(&ipa, zero_virt, zero_phys, wifi_qmp_send)`（8 KiB 设备可见内存；apps SMMU 处于旁路），主线是在 modem 启动之前的探测阶段做；MBA/PIL 开始启动 modem 时调 `coachz_ipa_modem_starting()` 并清掉我们的 SMP2P `ipa` 项（主线 `ipa_smp2p_notify_reset`）；GIC SPI 311 或轮询里调 `coachz_ipa_interrupt()`。另外 Wi-Fi 补丁打印 `ipa: setup ready` 时把 SHARED_MEM_SIZE 的高低 16 位读反了：主线 MEM_SIZE 在 15:0，MEM_BADDR 在 31:16，`mem_offset` = 8 × 高 16 位。

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

## 下一步

QMI 控制（SIM、注册、WDA 数据格式、WDS 绑定 mux 和拨号），以及给 rmnet 数据设备接 XNU 网络接口。
