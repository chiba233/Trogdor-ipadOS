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

本轮的候选检查不授权或执行写入。独立比较元数据与 SSPP geometry／XY／stride／format／unpack，非零表 PA 还须与 SSPP 一致。分别计算完整像素跨度和按锁定 libpayload 的 4 KiB GRANULE_SIZE 向上取整的分配跨度，候选基址限定其动态分配的 1 MiB 对齐；要求落在 Coreboot RAM，沿既有 FDT 运行时范围门排除保留区、动态 alloc-ranges、Coreboot、FDT、initrd 和固件保留区，并以完整分配跨度另行排除整个当前 Image。冲突或不一致时导出明确的 false 标志，不能把“发现完成”解释为“可绘制”或“屏幕可用”。

独立严格读取模式校验基础四字段、schema、完整发现／表与元数据标志、13 次成功读取／无故障／零硬件写入，以及所有必需原始读数、像素／分配跨度、范围标志和关系；独立计算 geometry／格式一致性、Image 冲突与最终候选关系。拒绝重复／缺失／矛盾字段和混入 XNU 记录；故障或早期拒绝记录可留存诊断，不能算完整发现通过。物理地址和原始记录保持私有，公开记录只写结论与公开产物摘要。

## 验证与后续

主机用合成表覆盖 checksum／截断／条目边界／重复／forward 循环与跨度、几何／GPIO／内存图拒绝和失败出参不变；私有原始表仅作本地补充。范围／快照关系与读取器使用独立反例。真实 AArch64 编译继续执行已分配 C 对象重定位及最终标量指令审计；TCG CPU 模型验证读取成功、受限 data abort、异常寄存器恢复和 HVC 返回，模型不代表 SC7180 MMIO 或真实 Linux。

原生镜像保留原 Linux、47 份 DTB 的其他属性、initrd、配置与命令行，沿既有签名／GPT／独立产物审查和批准的外置介质全量回读流程准备；手动启动后再按新模式验收。通过发现后，依据实际候选与冲突情况单独限定图案／缓存／TE／DSI 和 EC 背光恢复实验，之后才考虑最小文字输出或 XNU Video 输入。

实现顺序：纯解析／快照模型与反例 → 有界 FDT 入口／只读汇编与 CPU 模型 → 日志读取／独立构建开关 → 完整回归与审查 → 原生产物和测试介质。实现已完成，尚无真机显示发现结果。


## 主机验证与产物

2026-10-31：137 项完整测试通过，新增 11 项显示用例覆盖纯解析／快照／严格读取和实际 C 的 FDT→范围→日志路径；该路径测试的物理表获取与 MMIO 由桩提供，不作为硬件证据。私有原始表另经当前解析器验证，不进入公开 fixtures。

11 次 TCG 执行验证同一汇编读取与精确异常恢复：两个加载位置的 RAM 成功读取、真实 stage-1 data abort、失败出参不变、全部 callee-saved GPR／四项异常寄存器恢复及 HVC／Linux 入口桩；九种拒绝涵盖 FnV、写故障、错误 PC／FAR／模式／向量、未 armed、保存地址不符和错误 EC。TCG 没有执行 SC7180 外设或真实 Linux。另以当前源码重新执行已验收 version11，严格 40 字段通过且宏关闭的 ELF 不含显示符号。

四种基础探针构建通过。最终原生显示发现 Image 通过标量审计（4206 条指令／9 段）、七个实际 C 对象重定位检查，独立核对精确受保护 LDR、13 项白名单及源／产物摘要、ELF 导出、原始 Linux／47 份 DTB／initrd／配置／命令行、主备 GPT 与签名容器；未携带 XNU 负载。完整发现日志必须通过 `read_probe.py --display-discovery` 的全部 42 项必需字段；候选不可用仍可完成只读发现，不能算画图或显示恢复通过。


## 测试介质准备

运行时 `abac621`，设计 `55e6f66`。镜像 SHA-256 `07a4272f4e18488502f000eee3bfd644f054af634407069187f956bc2f014b29`，签名容器 SHA-256 `1f89f5a8660b9f22160e1a8bbbb454182d47ce1aa334fefb22aa868c75a0f3ca`。目标端独立核对镜像／容器／清单／两个读取器模块／公开密钥／固定哈希私有写入器的全部传输摘要，vboot 签名与 body 验证通过。重新确认可清空 USB 的 removable／容量并排除 root／boot／swap 后，完成全部 128 MiB 写入、同步与完整回读，摘要一致。没有写固件或内置盘，没有自动重启。

当前仅为发现介质准备完成，尚未执行原生显示寄存器读取。手动启动仍不点亮屏幕；随后原生 Linux 的完整 42 字段才能验收交接发现。若日志不完整，保持失败记录；即使完整且候选为真，也不能宣称显示恢复或 EC 背光已支持。
