# WFE 与单 CPU 拓扑检查点契约

设计：2026-10-30。基于已真机通过的 version7 完整早期 PE 返回，采用独立 version11，在 `arm_init` 的 `0xfffffff0079b3d4c` 替换下一条 `ml_get_boot_cpu_number()` 调用为单字 HVC。合并原始 WFE、chosen／stall 检查、4 KiB 页大小全局及 `ml_parse_cpu_topology` 返回验收，不进入后续 CPU 栈／线程／计时器中断或 VM 初始化。

## 输入与装载

使用相同固定源码内核、正常 CPU 初值、既有受限 ADT 和命令行；version11 限定当前 19.2 MHz timebase。version11 显式选择 mode7 基础语义并携带独立拓扑标记，不能落入 version9／10 的 SoC 诊断分支；旧 version7 的 PC、X0、父帧和读取条件保持独立。保留原生同步向量，不承诺异常现场回收。

PE 元数据后追加十个 64 位字段：topology_info、CPU 数组、cluster 数组、cluster_offsets、cpu_config_correct、arm64_eventi、PAGE_SHIFT_CONST、page_shift_user32、stall 全局的 KVA，以及 CPU reg 在 ADT 中的值偏移。源代码／生成码审计固定新增可达函数前缀和精确补丁指令。

所有新区域在 arena 写入前检查可读非执行数据段、跨度、对齐及彼此／既有区域不重叠；独立 C 树扫描确认 CPU reg 偏移和数值与当次启动 affinity 相符。初始化逐字节核对：topology_info 的 version=1 和两个固定数组指针、cpu_config_correct=1、arm64_eventi=UINT32_MAX，其余相应区域为零。拒绝错误偏移、长度、初值及重叠时不得修改 arena。实际固定内核将部分拓扑全局放在 SECURITY_READ_ONLY_LATE 的初始只读数据段；早期自有 arena 的块页表仍为可写，写权限由完整 bootstrap 表模型保障，不要求 Mach-O 描述预置写位，也不修改源段权限。

## 原生运行结果

基础捕获、完整 64 KiB 表池、硬件 AT、CPU／PE／DT／SoC／型号／时钟结构／配置、ADT 校验及 24 MiB arena 缓存清理保留。新停止点 X0=0；SP／FP 保持原 arm_init 父帧，X30 和父 SP−8 保存的 LR 为新停止点，其余全部父帧字节继续核对。CPU 数据对象在本轮未执行后续关联写入，应维持既有完整 PE 模型。

新全局模型独立构造：一个 SMP CPU／cluster，mask=1，物理标识来自独立验证的 ADT reg，正确 cpus／clusters／boot 指针、计数及默认缓存／PIO 值；完整比较 88 字节 info、四个 120 字节 CPU 槽、一个 72 字节 cluster、cluster_offsets 和全部新标志／页大小／stall 全局，包含 padding 与未用槽。DTCursor 仍为根节点首属性后，页大小为 12，stall 为零，configuration 标志保持 true。

入口时保存 CNTKCTL_EL1 至可信 EL2 快照，原有 TPIDR_EL0／TPIDRRO_EL0 快照继续使用。在 HVC 入口、C／AT 操作前单独捕获三个运行值。WFE 的期望从当次初始 CNTKCTL 计算 OR 结果；默认 19.2 MHz 输入对应 EVENTI=4，不能假定旧 CNTKCTL 为零。运行 TPIDR_EL0／RO 应均为逻辑 CPU／cluster 零的编码。

所有允许返回路径恢复 CNTKCTL 和原有状态；交接前再次读取这三个寄存器，逐项与可信入口快照一致才允许导出日志／进入 Linux。复用可信 EL2 符号，不从故障上下文选择恢复地址。

## 日志与验证

独立 `--xnu-topology` 严格读取：继承完整 PE 的 22 字段，加 WFE 状态、拓扑对象、系统寄存器恢复三个通过标志；要求 version11、正常 boundary／diagnostic 标记、精确新 ELR，以及三个寄存器的 before／live／after 字段，逐项验证恢复和值关系。旧 PE／SoC 和诊断读取模式不得将 version11 当作自身检查点通过。

验证覆盖所有新增输入初值／范围／重叠的写入前拒绝，完整新对象与未用槽逐字节破坏、全部新增寄存器和父帧／捕获／表池条件、读取器缺失／重复／矛盾／旧模式拒绝，实际 AArch64 对象绝对重定位审计和标量生成码检查。主机系统模型执行真实入口、原生源码、MMU 和返回，使用多组非零 CNTKCTL／TPIDR 初值，独立在 Linux 入口桩核对恢复；旧 mode6／7／9／10 同时回归。

源与主机证据、介质准备及真机验收分别记录。只在完整测试、系统模型、独立审查和产物校验通过后，按既有授权准备可清空外置介质并全量回读；手动启动后取得新版本完整记录才算硬件通过。version11 已实现，126 项完整主机测试、四种基础探针构建及七次 TCG 系统模型回归通过；三组新版本分别覆盖零值、非零寄存器初值和不同加载地址，旧 mode6／7／9／10 单独回归。最终原生产物通过 13 个实际 C 对象的重定位审计、标量生成码、原样负载／Linux／47 份 DTB／initrd／配置与 GPT 核对及独立审查。板级准入、日志导出与 Linux 入口在系统模型中仍由桩代替，不能当作 SC7180 物理缓存或真机执行证据。

2026-10-31：最终镜像已通过目标端签名／body 与全部传输摘要核对；重新排除 root／boot／swap 后，已写入既有可清空外置 USB，完整 128 MiB 同步及回读摘要一致。没有自动重启；随后手动启动取得完整日志，硬件 version11 执行验收已通过，见下文。真机日志必须通过 `read_probe.py --xnu-topology` 全部 40 项必需字段及精确返回位置／参数核对。


## 2026-10-31 真机验收

运行时 `3d6acfd`、介质准备记录 `2fa1a00`，本轮已全量回读的镜像手动启动后进入原生 Linux。8192 字节日志的头／终止符／零填充有效，`read_probe.py --xnu-topology` 全部 40 项必需字段唯一且通过；两次实时读取与私有副本字节相同。目标保存的镜像／容器／清单／读取器摘要与本轮产物匹配，记录未公开。

精确 HVC 返回位置、参数虚实关系、自有 arena／Image 对齐、affinity／19.2 MHz timebase 与负载一致；完整 PE、拓扑对象／未用槽／页大小／stall／父帧、完整表池／硬件 AT／24 MiB 缓存门通过，checkpoint=1、boundary=1、diagnostic return=0。WFE EVENTI=4，CNTKCTL 的运行值满足入口值 OR 0x4f；两个 TPIDR 运行值对应唯一逻辑 CPU／cluster。三个系统寄存器在交接前实际回读均恢复入口值，随后正常交接 Linux。

本次硬件的三个入口值均为零；非零初值恢复覆盖仍来自主机模型。WFE 验收指事件参数与访问／事件控制寄存器配置，不表示已验证睡眠唤醒或计时器中断；单 CPU 拓扑不表示其他核启动。线程／VM、后续 CPU／中断／SMP 和显示仍在范围之外。本轮仅采集证据、更新记录，没有改动运行时代码、重写 USB 或自动重启。
