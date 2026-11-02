# Wi-Fi：第一份未启用适配代码

代码在 `platform/coachz-prelink/pending/wifi/`，和其他驱动一样分阶段推进。这一轮按要求留在待接入状态，内核编译/接线交给接手者。没有改 `series`、`board.toml`、`kernel_source.py` 或正在使用的 `xnu_screen.py`，没有构建内核或内核对象、打包、写盘、连接设备、提交或推送。

## 路线和本轮范围

两条路线比较后，仍选择既有调研建议：参考同WCN3990的ios-flame协议层，单独替换SC7180平台层；不把面向PCI/MHI的Ath12kWifi当作SNOC硬件后端，也不搬整套Linux ath10k框架。

先把板级差异和控制面做成能调用的代码，而不提前发布一个假en0。首轮由只读SMEM探针和独立WLFW报文代码组成；MPSS冷启动仍是下一道实际硬门槛。详细接线见同目录 `README.md`，固定源码见 `SOURCES.md`。

| 阶段 | 当前实现 | 验收边界 |
|---|---|---|
| SMEM平台清点 | 已写：确认ADT范围与XNU内存窗口不重叠，解析公开partition table，仅读global/APPS-MODEM，读取incoming SMP2P及已有GLINK描述符 | 真机：global 和 apps-modem 分区读到，modem 启动后能看到它新建的条目 |
| WLFW控制报文 | 已写：QMI/TLV、indication registration、CAP、SC7180 MSA-info请求、严格响应匹配、能力字段、QRTR服务增删 | 主机固定报文通过；没有实际发送或收到固件响应 |
| MPSS 启动 | 已写（`coachz-wifi=4`，`coachz-prelink-wifi.patch`）：照主线 `qcom_q6v5_mss` 的 MBA 和 MPSS 加载、内存归属、IPA 供电和 rmtfs 共享；AP 侧 SMEM 分配（TCSR 互斥锁 3）和 SMP2P 条目 428 | 真机：modem 报告 ready 和 handover；约 40 s 后因初始化卡住报 fatal |
| 传输 | 已写：GLINK（AP 的 FIFO 479、版本和通道握手、intent）、QRTR（节点 1，hello 后通告 rmtfs）、rmtfs（QMI 服务 14，EFS 在内存里） | 真机：modem 不再超时，通告了 WLFW（69）等服务；还没向 WLFW 发过 QMI 请求 |
| WLFW 握手 | 已写：照 ath10k `qmi.c`，到板级数据下发和 FW ready | 真机：芯片 0x320、板 0x67，BDF 来自 `board-2.bin` 的 `bus=snoc,qmi-board-id=67`，收到 FW ready |
| WLAN 和 WMI | 已写：WCN3990 供电、WLAN 模式、12 个 copy engine、HTC，`wmi-tlv.c` 的 service ready 和 WMI_INIT | 真机：固件报告 READY，status 0 |
| 无线扫描 | 待做：vdev、HTT、扫描、真实BSS，以及系统网络接口 | 重复扫描与错误回收 |
| 联网与系统接入 | 待做：station/WPA2、ifnet/Apple80211、wifid/configd、DHCP和DNS | 系统网络栈联网，再验证设置操作和生命周期 |

真机（k78，只读）：TF-A 的 SMC64 SIP 调用可用，PAS 系列一个都没实现，`assign_mem` 实现了，所以 MPSS 只能走主线 trogdor 的 MBA 自认证（`qcom_q6v5_mss.c`，`qcom,sc7180-mss-pil`）。TCSR 0x01f60000 的 Q6/NC halt 空闲、无请求。`qdsp6sw.mbn` 有 65.6 MB，进不了 payload，需要另找传送方式；`mba.mbn` 283 KB 可以。

真机（k80）：AOSS QMP 通道已照 `qmp_open` 打开（链路、通道和应答都是 UP）；AP 侧 RPMh 写入通路可用（active TCS 发出 `mss.lvl=0`，10 µs 完成）。下一步：把 MPSS 固件送进内核，然后照 `qcom_q6v5_mss.c` 做 MBA 启动。

真机（k86～k95，详见 `docs/hardware-tests.md`）：mpss_mem 先于 mba_mem 交给 modem 后 PBL 通过；LTE 版的 mpss_mem 是 140 MiB（主线 `sc7180-trogdor-lte-sku.dtsi`，Debian 的设备树也是），给足之后 MBA 约 157 ms 就绪。`qdsp6sw.mbn` 的元数据和 30 个段认证通过，modem 运行，在 apps-modem 的 SMEM 分区里建了约 20 KiB 的条目，但没有置 ready 位：AP 侧的 SMP2P 出站条目和 GLINK 还没有，这是下一步。

真机（k81～k85）：MPSS 固件经内核分区 144 MiB 处的固件块送进内核，`mba.mbn` 和 `qdsp6sw.mbn` 的 sha256 都对。MBA 第一段（`coachz-wifi=3`：load_state、cx/mx/mss 最高档、五个 MSS 时钟）照常完成并撤回。MBA 第二段（`coachz-wifi=4`）把 `mba.mbn` 拷进 mba_mem，TF-A 的 `assign_mem` 把它交给 modem（返回 0），随后线程停在下一次 QDSP6SS 或 RMB 访问上，Q6 还没有启动。UniXNU 的 Wi‑Fi 页写的是 PCIe 上的 WCN7850（Ath12kWifi）；Coachz 的 WCN3990 由主线 `ath10k_snoc` 经 QMI 连到 modem 上的 WLAN 固件服务，所以先要启动 MPSS，之上再接 `nw_abi.h` 与 `WifiNet.cpp` 那几层。

## 实现里的目标板差异

- SMEM采用SC7180的`0x80900000/2MiB`，不是Pixel SM8150的SMEM地址。
- MSA报文只接受当前参考DTS的`0x94100000/2MiB`；调用前仍需确认实际FDT及内存所有权。这里只构造报文，未作权限分配或发送。
- 主机SID为`0xc0`、firmware SID为`0xc2`，不能套Pixel `0x640`。本轮未配置任何SMMU。
- Linux目标使用MSS PIL，并非只换Pixel PAS函数的地址；本轮不提供未经目标验证的启动序列。
- SMP2P：modem 写 item 435（MODEM→APPS，1→0），AP 写 428（主线 `smp2p.c` 把 `qcom,smem = <435>, <428>` 依次当作入站、出站）；GLINK为478/479/480。只读已有内容，不创建/消费/确认任何item。
- `SMEM v12 inventory status 0`只说明布局解析成功。状态位和旧item可能是残留，不能写成“固件ready”或“Wi-Fi启动成功”。

## 未启用状态与主机检查

执行 `python3 -B -m unittest discover -s tests -p test_wifi.py -v`。它只编译便携C夹具，不编译XNU代码；使用实际loader的ADT Node序列化检查helper，并在临时目录检查候选补丁。

测试防止把其他主机的受保护SMEM分区读进来；覆盖uncached/cached item、padding、incoming SMP2P、缺席/未初始化状态、WLFW固定字节序、结果码和事务/消息匹配、QRTR服务发现以及当前构建仍未启用Wi-Fi。没有用主机结果代替真机。

2026-11-01：上述4项unittest通过；其中便携C程序的7组协议/内存夹具全部通过。候选补丁在临时目录应用后，三个新C/header文件与pending原文件逐字节一致。没有编译XNU侧的`coachz_wifi.c`，实际内核构建和硬件行为仍待接手者验证。

文件来源和SHA/blob信息已放在 `pending/wifi/SOURCES.md`。没有导入固件、BDF、个人网络配置或其他设备校准；本轮只新增Wi-Fi专用文件，保留其他工作的未提交改动。
