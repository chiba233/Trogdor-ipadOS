# Wi-Fi：第一份未启用适配代码

代码在 `platform/coachz-prelink/pending/wifi/`，和其他驱动一样分阶段推进。这一轮按要求留在待接入状态，内核编译/接线交给接手者。没有改 `series`、`board.toml`、`kernel_source.py` 或正在使用的 `xnu_screen.py`，没有构建内核或内核对象、打包、写盘、连接设备、提交或推送。

## 路线和本轮范围

两条路线比较后，仍选择既有调研建议：参考同WCN3990的ios-flame协议层，单独替换SC7180平台层；不把面向PCI/MHI的Ath12kWifi当作SNOC硬件后端，也不搬整套Linux ath10k框架。

先把板级差异和控制面做成能调用的代码，而不提前发布一个假en0。首轮由只读SMEM探针和独立WLFW报文代码组成；MPSS冷启动仍是下一道实际硬门槛。详细接线见同目录 `README.md`，固定源码见 `SOURCES.md`。

| 阶段 | 当前实现 | 验收边界 |
|---|---|---|
| SMEM平台清点 | 已写：确认ADT范围与XNU内存窗口不重叠，解析公开partition table，仅读global/APPS-MODEM，读取incoming SMP2P及已有GLINK描述符 | 主机夹具通过；XNU侧尚未编译/实机运行 |
| WLFW控制报文 | 已写：QMI/TLV、indication registration、CAP、SC7180 MSA-info请求、严格响应匹配、能力字段、QRTR服务增删 | 主机固定报文通过；没有实际发送或收到固件响应 |
| MPSS/传输 | 待做：SC7180 MBA/PIL、权限和资源、SMEM锁/分配、SMP2P/GLINK握手、QRTR/QMI事务与超时 | 真实WLFW服务与有效响应 |
| 无线扫描 | 待做：正确BDF、firmware/host SMMU、CE、HTC/WMI/HTT、真实BSS | 重复扫描与错误回收 |
| 联网与系统接入 | 待做：station/WPA2、ifnet/Apple80211、wifid/configd、DHCP和DNS | 系统网络栈联网，再验证设置操作和生命周期 |

## 实现里的目标板差异

- SMEM采用SC7180的`0x80900000/2MiB`，不是Pixel SM8150的SMEM地址。
- MSA报文只接受当前参考DTS的`0x94100000/2MiB`；调用前仍需确认实际FDT及内存所有权。这里只构造报文，未作权限分配或发送。
- 主机SID为`0xc0`、firmware SID为`0xc2`，不能套Pixel `0x640`。本轮未配置任何SMMU。
- Linux目标使用MSS PIL，并非只换Pixel PAS函数的地址；本轮不提供未经目标验证的启动序列。
- SMP2P incoming为item 428、MODEM→APPS（1→0）；GLINK为478/479/480。只读已有内容，不创建/消费/确认任何item。
- `SMEM v12 inventory status 0`只说明布局解析成功。状态位和旧item可能是残留，不能写成“固件ready”或“Wi-Fi启动成功”。

## 未启用状态与主机检查

执行 `python3 -B -m unittest discover -s tests -p test_wifi.py -v`。它只编译便携C夹具，不编译XNU代码；使用实际loader的ADT Node序列化检查helper，并在临时目录检查候选补丁。

测试防止把其他主机的受保护SMEM分区读进来；覆盖uncached/cached item、padding、incoming SMP2P、缺席/未初始化状态、WLFW固定字节序、结果码和事务/消息匹配、QRTR服务发现以及当前构建仍未启用Wi-Fi。没有用主机结果代替真机。

2026-11-01：上述4项unittest通过；其中便携C程序的7组协议/内存夹具全部通过。候选补丁在临时目录应用后，三个新C/header文件与pending原文件逐字节一致。没有编译XNU侧的`coachz_wifi.c`，实际内核构建和硬件行为仍待接手者验证。

文件来源和SHA/blob信息已放在 `pending/wifi/SOURCES.md`。没有导入固件、BDF、个人网络配置或其他设备校准；本轮只新增Wi-Fi专用文件，保留其他工作的未提交改动。
