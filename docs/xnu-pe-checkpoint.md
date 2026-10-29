# XNU 完整早期平台返回检查点

version=7 沿既有真实源码和 ADT 执行完整 PE_init_platform(FALSE)，在 arm_init 的调用返回处以单条 HVC 停止，合并 SoC 属性、CPU 遍历／频率数据、根型号和早期调试配置验收。沿用原厂 FIT、正常零 CPU 初值及 arena 缓存清理后交接 Linux；不进入下一条 timer 配置调用、MMIO、中断或 SMP。

比较仅推进一个属性、完整 FALSE 平台返回和继续进入 VM／设备初始化。选择完整 FALSE 返回：这段调用链可限定为内存解析与自有全局／栈写入，可以减少真机轮次；继续进入设备初始化需要另行审计。

## 固定输入与已知缺口

保留实际最小 ADT；其 arm-io ranges 父地址非零，原始机器识别会遍历唯一 running CPU。装载前由独立 Python／C 解析器限定树大小、节点／深度、规范键／名字、唯一必要节点、正常 state、4 字节 timebase、24 字节 ranges 及有界型号／device_type。只允许审计过的根、chosen、CPU 属性；拒绝会启用未审计分支的可选属性。命令行固定为既有默认值。

现有 ADT 没有 bus／memory／peripheral／fixed／CPU frequency 属性，原始代码最终把相关字段置零；仅 timebase 与 dec 使用实际计数器频率。AArch64 UDIV 的零分母结果为零，不触发异常。这是当前输入的兼容性缺口，不代表硬件频率是零，也不构成时钟驱动验收；不修改 ADT 制造绕过路径。

## 返回契约

保留 mode6 的 PE／DT 全局检查，改用本轮最终根查找游标。完整核对 SoC 缓存指针／字符串／物理基址、平台信息块中的型号和完整时钟结构、未写字段、配置 bitmask=3、debug_enabled=0 与 degradation factor=1。实际 ADT 复制并填入当次 DRAM 字段后计算 FNV-1a 校验值，返回时逐字节重新计算，检测意外修改；它是内存一致性检查，不是密码学认证。

最终只保留 arm_init 活动父帧；子调用帧允许退休残留，父帧低两字仍为 PE 保存的 FP／LR。核对全部活动帧、4 KiB 栈底 guard、x19=CPU 对象、x20=const_boot_args、SP／FP／LR、精确返回 PC 和最终 x0=零扩展 kError。保留完整 CPU、参数复制、表池、硬件 AT 与缓存维护检查。

## 实施与验证

固定完整源内核及实际执行函数哈希；扩展独立 version=7 元数据和只读／可写区域的装载前边界、初值及不重叠检查。所有输入检查必须先于 arena 写入。旧六模式保持可独立构建／解析。

主机回归覆盖：独立树契约、未审计分支和越界拒绝、写入前拒绝、全部新全局／活动父帧／guard 的逐字节破坏拒绝、ADT 修改检测、全部寄存器和严格日志门限、真实源码单字补丁及实际包装器交叉编译。独立审查后生成签名普通文件，外置介质重新核对属性并完整回读；不自动重启。主机成功与真机结果分开记录，原始设备数据继续保持私有。


## 主机验证与读取

98 项主机回归通过，四种原有探针仍可构建。新增用例覆盖独立 Python／C 解析结果对照、确定性逐字节畸变与截断、未审计可选分支、任意层级重复兄弟节点拒绝、装载前拒绝、完整新全局／父帧／栈底 guard 及 ADT 的逐字节破坏、全部捕获字段、真实源码单字补丁和旧六模式兼容。独立源码与实现审查通过；最终包装器 6366 条指令、13 段保持标量约束，Image 仍占有 74 MiB。

```sh
python3 tools/xnu_checkpoint.py --kernel /path/to/source-kernel \
  --kernel-report build/xnu-source/source-preparation.json \
  --adt /path/to/generated.adt --output build/xnu-pe/payload.bin --pe-return
python3 tools/linux_log.py --source /path/to/native-linux.kpart \
  --futility /path/to/futility --key-dir /path/to/developer-keys \
  --xnu-checkpoint build/xnu-pe/payload.bin --output build/xnu-pe-usb
```

工具只生成普通文件。返回 Linux 后运行 `sudo python3 tools/read_probe.py --xnu-pe`，要求既有平台检查点的 19 字段及以下三个唯一且为 1 的字段，共 22 字段：

```text
XNU SoC properties verified=0x0000000000000001
XNU clock info verified=0x0000000000000001
XNU PE return verified=0x0000000000000001
```

字段成功后还须独立核对 version=7、精确 arm_init 调用返回 PC、参数和清单，不能用此前首次 ADT 查找结果替代。clock info 字段仅表示结构内容符合当前受限输入，不表示缺失频率已补齐或硬件时钟已配置。

| 产物 | SHA-256 |
| --- | --- |
| source kernel | 6b406b8cd4ee5a6c11793256d8cadc5453365e8e8edb5f487953e02e259c7e9b |
| instrumented PE-return kernel | 215b6ee4ea6ad3044c4cc391a14c284c1cfd3627ee648408be908cc9a192275f |
| checkpoint payload | b2e73dfbfdae5ce5033ea8bf24de37ef0981a9fff00315e5b6ffda5d0775683f |
| linux-log.kpart | ca3685e83a043df599c1dee4e73de54ce5942ea4f8c9a695b663049cbeb2da9d |
| linux-log.img | b15cc8ac07b6823835a7edc731b4daa18323c21c3d9da13f19267cd35549733e |


主机与目标端签名／body、实际主备 GPT CRC／属性／内嵌容器及最终代码清单匹配；Linux、47 份完整 DTB、initrd 和命令行保留。传输哈希与外置介质全部 128 MiB 回读一致。没有写固件或内置盘，没有自动重启。以上为主机与介质准备结果，本轮真机执行仍待手动启动后取得全部 22 字段及独立清单核对。


## 黑屏后的诊断返回对照

首次 version=7 尝试持续黑屏，手动恢复内置 Linux 后没有本轮设备树日志，不能确认 XNU 停止位置或记为通过。主机侧使用实际补丁内核重放原始 CPU 初始化及完整 PE 指令，能够到达精确返回位置，SoC／PE／DT／型号／时钟／配置和活动父帧均与校验器相符；这只是主机对照，不是硬件结果。

下一份对照保留相同源码 XNU 负载、ADT、检查点和所有严格成功条件，只改变 version=7 在返回门后的诊断策略：完整捕获现场（含 PC、SP、GPR、MMU、硬件 AT、缓存清理计数）及全部 64 KiB 页表池仍须通过；若之后的普通内存数据校验失败，则保存失败字段并照常恢复状态、交接 Linux。检查点成功字段仍为零，不得视为通过。新增 return boundary verified 和 diagnostic return 字段分别表示这个严格返回门是否满足、是否为失败诊断返回。

如果未到 HVC、完整现场或页表池检查失败，仍停机；这份对照不能保证所有早期故障都能取回日志。没有改变原始 XNU 执行路径，也没有放行后续 timer／设备初始化。旧六模式保持失败即停机。


诊断对照通过 99 项主机回归与独立审查。新增用例逐项破坏全部 27 个捕获字段、已用及未用表项，确认返回门仍拒绝；旧六模式也拒绝这一额外返回路径。读取测试拒绝矛盾／重复诊断标记。成功读取仍要求原有 22 字段全通过；附加标记若存在，必须为 return boundary verified=1、diagnostic return=0。diagnostic return=1 只表示取得失败日志。

| 诊断对照产物 | SHA-256 |
| --- | --- |
| linux-log.kpart | e6a2ab5e45d0120334ca102e4c11d9ad1d7069a2ff66763fb0b41e3c3d03f62c |
| linux-log.img | 81468498db2ac78c6f9ffbb5bf07b2404c65422bb4d2d3c544cc7f2d37bf67dc |

源码 XNU payload SHA-256 仍为 `b2e73dfbfdae5ce5033ea8bf24de37ef0981a9fff00315e5b6ffda5d0775683f`，与首次黑屏尝试逐字节相同。

诊断对照的包装器为 6409 条标量指令、13 段，仍占有 74 MiB。2026-10-29 已完成目标端传输哈希、签名／body 校验，以及本诊断镜像在外置介质上的全部 128 MiB 写入与回读，哈希一致；没有写固件或内置盘，没有自动重启。介质记录见 [硬件记录](hardware-tests.md)。本对照的实际启动日志仍待取得，不能沿用首份镜像的回读或启动结果。


2026-10-30 后续定位到 mode7 起装载前 ADT 解析器的绝对字符串指针缺陷，故障日志标签也有相同问题；已改用内联数组并加入链接前对象审计。系统 CPU 模型的对照与严格读取通过，实际硬件停止位置和本检查点验收仍未确定，见 [诊断修复记录](xnu-diagnostics.md)。


2026-10-30 version9 的 SoC 中点已经严格 20 字段真机验收，下一轮重新准备本节原始 version7 负载并使用修复后的包装器。主机完整 CPU 模型的全部 22 个 PE 字段已通过，完整平台返回仍待独立真机日志。现有 ADT 缺失频率的兼容性限制继续保留，见 [后续硬件记录](hardware-tests.md)。

该原负载／修复包装器镜像已完成目标端签名与传输验证，以及全部 128 MiB 介质写入、同步和完整回读，摘要见 [最新准备记录](hardware-tests.md)；仍待手动真机 22 字段验收。


## 2026-10-30：修复后的原负载真机通过

运行时代码 `93c9e22`、介质准备记录 `c1adc3c`，镜像 SHA-256 `a4453bb9c721dd26e6d7035a1459dc7c0e9b8753e4c629a4354e7ced49278046`。原 version7 payload 与首轮逐字节相同。沿已全量回读介质手动启动，原生 Linux 收到完整 8192 字节日志；严格读取的全部 22 个必需字段唯一且通过。精确 arm_init 中的 PE 调用返回位置、参数虚实关系、64 字节缓存行 × 393216 行覆盖 24 MiB arena 均与清单／负载匹配，boundary=1、diagnostic return=0，状态恢复后完成 Linux 交接。设备属性与私有副本重复读取摘要一致，目标镜像／容器／读取器和本轮清单匹配；未公开原始记录或其摘要。

这确认受限 ADT、正常 CPU 初值下的完整早期 PE 调用及其 SoC／PE／DT／CPU／型号／时钟结构／配置／父帧校验。当前缺失频率仍为零，不代表时钟驱动或设备功能已支持；timer／WFE、CPU 拓扑、线程、VM、中断和 SMP 仍未执行。此前黑屏轮次的精确停止位置继续未知，不据本次成功改记历史结果。后续初步审计见 [post-pe-audit.md](post-pe-audit.md)。
