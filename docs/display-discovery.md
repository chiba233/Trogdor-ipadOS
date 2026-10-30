# 独立显示发现探针契约

设计：2026-10-31。基于已审计的 [固件显示复用路线](display-reuse-audit.md)，本轮实现该路线的第一步：原厂交接后的 Coreboot 表解析与受限只读 MMIO 快照，再经既有 EL1／HVC／EL2 路径返回原生 Linux。该独立镜像不携带或执行 XNU，不改动已验收 version11 的含义。

## 本轮选择

直接填 boot_args.Video 会跨入尚未验收的 XNU VM／console；直接恢复显示画图则需要已确定的真实 scanout 和内存归属。先做只读发现可以取回这些缺失输入，且无需新 EC 事务、显示／GPIO 写入或 framebuffer 修改。原始表的 PA 允许为零：它只提供元数据，实际候选地址由当次 SSPP 读数提供，绝不将零地址用于绘制。

## 有界输入

沿完整 FDT 验证 Coachz／SC7180 身份、地址与大小 cell、firmware identity ranges，以及 Coreboot 表／CBMEM 两个区间。限制为低 4 GiB 的 CBMEM、最大 2 MiB，首表必须完整位于 CBMEM 且不与当前 Image／FDT 重叠；不搜索任意 RAM。

纯 C 解析器按小端字段读取、检查 24 字节 LBIO 头及整个记录区的 IP checksum、长度、条数、严格记录边界与最终游标。最多跟随三个完整表，forward 必须为独立唯一记录、指向同一已限定 CBMEM，循环／越界均拒绝。内存图、framebuffer 与 GPIO 表唯一；内存图最多 64 段，验证非零无溢出且互不重叠。未知 tag 可跳过，未知 memory 类型不当作 RAM。错误不得向调用者交付部分有效描述。

限定 32 bpp、R16/G8/B0/保留24 的各 8-bit 通道、有效几何／stride、方向 0..3、完整帧跨度不超过 64 MiB；不硬编码面板尺寸。backlight 项须唯一、极性有效、port 与固定 Coachz GPIO12 的 TLMM 地址一致，才允许读取该 GPIO。只保存显示所需字段，不导出其他固件表内容。

## 只读快照与异常边界

只读取 13 个已审计寄存器：TE、DSI control、SSPP source／output size、source／output XY、source PA、stride、format、unpack，backlight GPIO configuration／in-out，以及 EC QUP1 SE0 的协议标识。没有 SPI 事务、FIFO 读取、IRQ 清除、GPIO 设置、显示控制写入或 framebuffer 写入。

这 13 次 MMIO 读取通过同一汇编原语：只在该精确 LDR 指令、EL1h 同步 data abort、读访问、FnV=0 且 FAR 等于该次输入地址时，返回读取失败。其他异常保持原停止路径，不伪造成功。每次调用独立置位／清除 guard 并核对保存的读取地址，不改变 callee-saved 通用寄存器；保存并恢复 ESR／ELR／FAR／SPSR_EL1；失败不写出参，停止后续 MMIO，日志标明不完整再沿普通 HVC 返回。成功返回路径不依赖外设、堆、锁或串口。Coreboot 表本身仍为普通有界 RAM 读取，依赖原厂 FDT 元组真实指向 CBMEM；上述保护不捕获伪造表指针的异常，也不证明任意外部 FDT 安全。

## 候选可写性与日志

本轮的候选检查不授权或执行写入。独立比较元数据与 SSPP geometry／XY／stride／format／unpack，非零表 PA 还须与 SSPP 一致。分别计算完整像素跨度和按锁定 libpayload 的 4 KiB GRANULE_SIZE 向上取整的分配跨度，schema 1 的候选基址门要求 1 MiB 对齐，这是对锁定源码动态分配路径的误读，见下方[分配与归属核实](#2026-10-31真实分配与保留归属核实)；要求落在 Coreboot RAM，沿既有 FDT 运行时范围门排除保留区、动态 alloc-ranges、Coreboot、FDT、initrd 和固件保留区，并以完整分配跨度另行排除整个当前 Image。冲突或不一致时导出明确的 false 标志，不能把“发现完成”解释为“可绘制”或“屏幕可用”。

独立严格读取模式校验基础四字段、schema、完整发现／表与元数据标志、13 次成功读取／无故障／零硬件写入，以及所有必需原始读数、像素／分配跨度、范围标志和关系；独立计算 geometry／格式一致性、Image 冲突与最终候选关系。拒绝重复／缺失／矛盾字段和混入 XNU 记录；故障或早期拒绝记录可留存诊断，不能算完整发现通过。物理地址和原始记录保持私有，公开记录只写结论与公开产物摘要。

## 验证与后续

主机用合成表覆盖 checksum／截断／条目边界／重复／forward 循环与跨度、几何／GPIO／内存图拒绝和失败出参不变；私有原始表仅作本地补充。范围／快照关系与读取器使用独立反例。真实 AArch64 编译继续执行已分配 C 对象重定位及最终标量指令审计；TCG CPU 模型验证读取成功、受限 data abort、异常寄存器恢复和 HVC 返回，模型不代表 SC7180 MMIO 或真实 Linux。

原生镜像保留原 Linux、47 份 DTB 的其他属性、initrd、配置与命令行，沿既有签名／GPT／独立产物审查和批准的外置介质全量回读流程准备；手动启动后再按新模式验收。通过发现后，依据实际候选与冲突情况单独限定图案／缓存／TE／DSI 和 EC 背光恢复实验，之后才考虑最小文字输出或 XNU Video 输入。

实现顺序：纯解析／快照模型与反例 → 有界 FDT 入口／只读汇编与 CPU 模型 → 日志读取／独立构建开关 → 完整回归与审查 → 原生产物和测试介质。实现与首轮真机只读发现均已完成，结果及候选限制见下方验收。


## 主机验证与产物

2026-10-31：137 项完整测试通过，新增 11 项显示用例覆盖纯解析／快照／严格读取和实际 C 的 FDT→范围→日志路径；该路径测试的物理表获取与 MMIO 由桩提供，不作为硬件证据。私有原始表另经当前解析器验证，不进入公开 fixtures。

11 次 TCG 执行验证同一汇编读取与精确异常恢复：两个加载位置的 RAM 成功读取、真实 stage-1 data abort、失败出参不变、全部 callee-saved GPR／四项异常寄存器恢复及 HVC／Linux 入口桩；九种拒绝涵盖 FnV、写故障、错误 PC／FAR／模式／向量、未 armed、保存地址不符和错误 EC。TCG 没有执行 SC7180 外设或真实 Linux。另以当前源码重新执行已验收 version11，严格 40 字段通过且宏关闭的 ELF 不含显示符号。

四种基础探针构建通过。最终原生显示发现 Image 通过标量审计（4206 条指令／9 段）、七个实际 C 对象重定位检查，独立核对精确受保护 LDR、13 项白名单及源／产物摘要、ELF 导出、原始 Linux／47 份 DTB／initrd／配置／命令行、主备 GPT 与签名容器；未携带 XNU 负载。完整发现日志必须通过 `read_probe.py --display-discovery` 的全部 42 项必需字段；候选不可用仍可完成只读发现，不能算画图或显示恢复通过。


## 测试介质准备

运行时 `abac621`，设计 `55e6f66`。镜像 SHA-256 `07a4272f4e18488502f000eee3bfd644f054af634407069187f956bc2f014b29`，签名容器 SHA-256 `1f89f5a8660b9f22160e1a8bbbb454182d47ce1aa334fefb22aa868c75a0f3ca`。目标端独立核对镜像／容器／清单／两个读取器模块／公开密钥／固定哈希私有写入器的全部传输摘要，vboot 签名与 body 验证通过。重新确认可清空 USB 的 removable／容量并排除 root／boot／swap 后，完成全部 128 MiB 写入、同步与完整回读，摘要一致。没有写固件或内置盘，没有自动重启。

介质准备后的首轮只读发现已通过下方验收。该镜像仍不点亮屏幕；完整发现、可写候选及显示恢复是不同结论。


## 2026-10-31：独立只读显示发现真机验收

运行时 `abac621`、准备记录 `476ce2e`，使用前述同一镜像与签名容器。目标保存的镜像／容器／清单／两个读取器模块与本地最终产物一致；清单的源码及产物摘要另行重算核对。原生 Linux 收到完整 8192 字节 probe-log，两次独立实时读取字节一致，私有副本通过当前 `read_probe.py --display-discovery` 全部 42 个严格必需字段。入口 EL2、探针 EL1、stage=3、交接 EL2 均匹配。

Coreboot 表与 framebuffer／GPIO 元数据有效，13 次固定 MMIO 读取完成、故障为零、硬件写入为零，随后正常交接 Linux。TE 与 DSI control 均为零，与源码停机路径相符；本轮不推断其余时钟、PHY、PLL、面板或 SMMU 状态，也没有执行 XNU、SPI 事务、framebuffer 或背光操作。

几何、XY、stride、format、unpack 与有效元数据逐项匹配；原始表 PA 为零，实际 scanout 来自 SSPP。Coreboot RAM 判定为真，与当前 Image 的完整分配范围不冲突。但实际 scanout 仅满足 4 KiB、未满足当前候选的 1 MiB 基址对齐，故现有“scanout consistent”合取结果为假；这不能误写为几何或像素格式错误。FDT 范围门也返回假：复用的 `xnu_range_allowed()` 整体排除固件加载与运行区，当前 scanout 落在该排除带内。该结论并不能单独说明它与某个具体活跃拥有者冲突，仍须检查当次 FDT／Coreboot 分配和完整保留范围。

最终可写候选为假，严格读取器正确接受“只读发现完成、候选不可用”。当时把 1 MiB 基址当作锁定分支的动态分配行为；下一节核实确认这是误读。不能先放宽门限再直接绘制。原始物理地址、表、日志及其摘要保持私有。

本轮仅读取、保存证据并更新脱敏记录，没有改写运行时或测试介质，没有写固件或内置盘，没有自动重启；无代码变更，不重复已通过的 137 项测试。该结果验收发现与 Linux 返回，不验收图案可见、背光、XNU Video 或完整系统启动。


## 2026-10-31：真实分配与保留归属核实

本节只读，没有写入硬件、改动运行时或自动重启。证据按来源分开记录。

**源码。** 固定 libpayload 的 [`mmu_get_new_range`](https://chromium.googlesource.com/chromiumos/third_party/coreboot/+/29f90de5f72c645a00146e04789896766f28e783/payloads/libpayload/arch/arm64/mmu.c) 先把区间末端向下对齐，再减去长度得到基址。注释称基址也随之对齐，这只在长度是对齐值倍数时成立；完整帧的 4 KiB 分配跨度不是 1 MiB 倍数。因此动态路径的正确关系是“基址加分配跨度按 1 MiB 对齐”，基址本身只到 4 KiB。schema 1 的 1 MiB 基址门来自对这段源码的误读，不是固件差异的证据。同一路径在 32 MiB DMA 区之后分配，只接受 payload `_end` 之上、4 GiB 之下的范围，所以动态 framebuffer 必然落在 `xnu_range_allowed()` 的整体排除带内；这道门对该对象不可能为真。

**模型。** 用私有 Coreboot 内存表按上述逻辑重放分配，结果与真机 SSPP 基址逐位相等；payload `_end` 取多个值结果不变，DMA 大小改为 16 或 64 MiB 则不再相等。这支持当前路径符合锁定源码，不能证明已安装固件二进制与之相同。

**硬件。** 从产生该日志的同一次启动读取设备树，其中的 probe-log 与私有记录逐字节相同。`/memory` 完整覆盖分配范围，并与按 Depthcharge [`update_memory`](https://chromium.googlesource.com/chromiumos/platform/depthcharge/+/719118f5e36ea312c33b8f005798b1488f9ec2d1/src/boot/fit.c) 从内存表推导的结果完全一致；10 个 reserved-memory 节点都不与它重叠，相对 FIT 内 DTB 只多固件加入的 ramoops；没有 simple-framebuffer 节点。Image、initrd 与 FDT 位于其下方，Coreboot 表与 CBMEM 位于其上方。

**主机。** 以该设备树重建 FDT（memreserve 由上述模型生成），运行实际 C 范围门：未改动时返回假，与真机一致；仅在临时副本中移除整体排除后返回真。尾页预留、上下紧邻预留、静态节点、alloc-ranges、CBMEM、initrd、`/memory` 之外共八组反例结果符合预期。

**结论。** 已识别的拥有者只有 Depthcharge／libpayload 结束运行后遗留的旧分配；交接给内核的内存表没有为它预留，Linux 会把它当普通 RAM。因此后续恢复 scanout 必须在交接前停止 TE，XNU 也必须自行排除这段范围。整体排除带继续作为 XNU arena 放置策略保留，不再用来判断现存显示对象的归属。

**未证实。** 当次 memreserve 未直接读取（`/sys/firmware/fdt` 需要 root，本轮未用）；已安装固件二进制未比对；payload `_end` 的实际值未知；SMMU、MDP、缓存及其他 DMA 主设备状态没有证据。以上都不授权写入，也没有可见画面证据。


## schema 2：元数据、分配关系与显示归属分开判断

schema 1 把多个条件合成 `scanout consistent` 和 `FDT range allowed` 两位，结果容易误读。schema 2 分开输出；13 个寄存器读取及其异常保护不变，共 45 个严格字段。

| 字段 | 含义 |
| --- | --- |
| `Display metadata consistent` | SSPP geometry／XY／stride／format／unpack 与元数据一致，scanout 非零，表 PA 为零或相等；不含对齐条件 |
| `Display allocation relation` | 表 PA 为零，且 scanout 加分配跨度按 1 MiB 对齐。非零表 PA 走另一条源码路径且没有真机证据，判假 |
| `Display allocator window` | 完整分配范围位于 Depthcharge 基址与 4 GiB 之间，排除 FIT 和 FDT 缓冲 |
| `Display Coreboot RAM` | 完整分配范围落在单个 Coreboot RAM 项内 |
| `Display FDT owners clear` | `fdt_range_clear()`：完整位于 `/memory`，且不碰 FDT 自身、Coreboot 两元组、memreserve、initrd、reserved-memory 的 reg／alloc-ranges |
| `Display Image conflict` | 与当前 Image 的完整范围重叠 |
| `Display ownership verified` | 窗口、RAM、FDT 三项为真且无 Image 冲突 |
| `Display candidate writable` | 元数据、分配关系与归属三项都为真 |

运行时取不到 payload `_end`，窗口下界只能用 Depthcharge 基址；落在 Depthcharge 自身范围内的情况不会被这一项发现。

`xnu_range_allowed()` 改为先做整体排除、再调用 `fdt_range_clear()`，判断结果和顺序不变，继续只用于 XNU arena。

候选仍只表示通过只读判断，不授权写入，也不表示屏幕可见；图案实验需要单独的契约。schema 1 记录继续按原规则读取，不重新解释：`read_probe.py --display-discovery` 只接受 schema 2，历史记录使用 `--display-discovery-schema1`，两种 schema 互相拒绝。schema 2 不需要为验证规则单独启动一轮，下一个显示镜像会带上这些字段并取得真机记录。
