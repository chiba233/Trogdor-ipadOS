# 原生 Linux 接收探针日志：实现契约

日期：2026-10-27。该诊断路线已选定。独立探针先执行，Linux 仅接收日志；它不替代原生 iPadOS 的后续引导目标。

## 数据流

原厂 Depthcharge（EL2）→ 独立探针（EL1）→ 专用 HVC → 探针 EL2 返回点 → 已知可用 Linux（EL2）。

Linux 保持从原厂 EL2 入口执行，避免把探针的最小 EL1 环境冒充完整 Linux／多核启动契约。链式版本要求 EL2 入口；其他入口停止，不猜测降级方案。

在每个原有完整 DTB 的 chosen 节点预留 8192 字节的 `trogdor-ipados,probe-log` 属性。探针完成 EL1 诊断后使用 HVC #0x5452 返回自己的 EL2 向量，仅接受 lower-EL AArch64 同步向量、HVC64 类型与精确 immediate。EL2 端确认 stage=3、记录的 current EL=1、实际 EL=2，追加交接级别并把文本复制到预留属性。

随后恢复此次探针修改的 EL2 控制寄存器及原 VBAR，使用 x0=FDT、x1..x3=0 跳转到嵌入的原始 Linux Image。Linux 不经过 EFI，不使用虚拟机，也不从探针访问存储或 EC。HVC 不可用、异常或日志出口无效时停止，不能报告 Linux 已启动。

## 产物与内存

以原有签名 Linux 容器为私有输入，经 futility 验证并提取 FIT。保留原有配置选择、完整 DTB、initrd 与内核命令行；只替换 kernel data／compression、为 DTB 增加日志属性，并重新计算相关 FIT hash。Linux 原始 Image 字节保持不变。

Linux Image 嵌入 shim 后方的 2 MiB 对齐位置，要求其 text_offset=0、little-endian、支持任意 2 MiB 放置；不满足时拒绝构建。wrapper 的 Image memory size 覆盖其日志、栈以及 Linux image_size，不能只用 Linux 文件长度代替内存占用。Depthcharge 继续负责整个 wrapper 的定位和缓存交接。

原有 16 MiB kernel 分区容不下该诊断负载。单独生成 128 MiB 普通 GPT 文件，包含一个 96 MiB kernel 分区；旧镜像格式保持默认参数不变。写入介质仍是独立、已明确授权且带目标盘保护的操作。

Linux 容器、完整 DTB、initrd 和实际日志仅保存在忽略目录，不进入公共仓库。公共代码只包含构建／校验逻辑和日志读取工具，不包含设备转储或 Linux 二进制。

## 验证

主机检查：源签名、FIT hash、DTB 全属性／reservations／boot CPU／版本保留、日志容量、Linux Image ABI、wrapper ELF 标量指令、payload 字节、内存范围、命令行原样传递、签名／GPT 布局。还需验证非链式四种探针行为没有改变。

硬件成功标准是从外置诊断镜像进入 Linux，并通过设备树读取此次探针写入的非空日志；日志必须含 current EL=1、stage=3 与 handoff EL=2。Linux 启动但属性为空不算成功。2026-10-27 已满足这些硬件成功条件，见下方记录。

## 构建与读取

```sh
python3 tools/linux_log.py --source /path/to/existing-linux.kpart \
  --futility /path/to/futility --key-dir /path/to/devkeys
```

输出目录默认 `build/linux-log`，必须尚未存在。可用 `--output` 选择新的目录。该工具先验证输入签名及 FIT hash，提取原 Linux Image、完整设备树和 initrd，编译 wrapper，并用原命令行签名生成 `linux-log.kpart` 与 `linux-log.img`。输入只能是现有原生 Linux 容器，工具不会猜测内置分区或读取设备。

从外置诊断镜像启动 Linux 后，在目标系统运行：

```sh
python3 read_probe.py
```

也可直接读取 `/sys/firmware/devicetree/base/chosen/trogdor-ipados,probe-log`。读取器要求完整容量、非空日志头、唯一的 EL／stage 字段与预期值；空白属性、截断、重复或错误级别均拒绝，不能把普通 Linux 启动误判为探针成功。

## 已完成主机验证

- 29 项回归通过，包含合成 Linux Image 的真实交叉链接及 HVC 指令检查。
- 原有四种独立探针均重新构建；reset-el1 Image 与既有匹配对照的原版基准字节一致。
- 实际输入包含 47 份完整 DTB；编解码与加入日志属性后保留全部其他语义属性、空节点、内存保留表、boot CPU 和兼容版本。
- 实际产物的原 Linux 字节、initrd、FIT configurations 与命令行保持一致，签名与 body 验证通过。
- wrapper 的 Linux 起点为 0x200000，文件大小 32569856 字节，Image memory size 为 33357824 字节，覆盖原 Linux 的完整内存占用；wrapper 执行范围 1983 条指令通过标量检查。
- 独立源码和产物审查未发现可操作问题。后续真机结果见下方记录。

以上是主机检查；与下方实际硬件观察分开记录。

目标系统的 futility 已再次验证签名／body，cgpt 确认 128 MiB 文件中的主备 GPT 和 96 MiB kernel 分区。外置介质写入后的全部 128 MiB 回读 SHA-256 为 `a533cc917bdd55a54c24b6f67e0d83e671890330e1dee989dea17af2c48be8a4`，与待测镜像一致；写入校验本身不能证明 Linux 交接成功；该结论来自后续日志读取。

## 2026-10-27：原生 Linux 接收成功

提交 `bb29e0e` 的外置镜像已交接原生 Linux，并读回完整 8192 字节日志。读取器确认 entry EL=2、current EL=1、stage=3、handoff EL=2；本地副本与运行中设备树属性的 SHA-256 一致。首阶段探针验收通过。镜像哈希、采集方式与证据边界见 [硬件记录](hardware-tests.md#2026-10-27原生-linux-接收成功)。

原始日志私下保存。该结果不包含 XNU／iPadOS 装载，也不代表独立探针已恢复显示或验证异常日志通道。


## 独立显示发现模式

在上述构建命令增加 `--display-discovery`，生成不携带 XNU 的只读显示交接快照镜像；该选项与 `--xnu-checkpoint` 互斥。部署读取器时须同时保留 `read_probe.py` 和 `read_display.py`，启动后用 `python3 read_probe.py --display-discovery` 验收（schema 2，45 字段；历史 schema 1 记录用 `--display-discovery-schema1`）。完整契约和候选／可见性区别见 [显示发现](display-discovery.md)。默认模式与已有 XNU 模式继续分别验收；显示标记不得混入 XNU 成功或诊断记录。
