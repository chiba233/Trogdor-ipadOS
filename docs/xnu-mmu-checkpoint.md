# XNU MMU 检查点设计

沿用已验收的原厂 FIT → EL1 XNU → HVC → EL2 Linux 路线。本轮目标是在真实源码内核开启 MMU 后、进入 C 初始化前取得可校验日志。实现、主机回归与独立审查已通过，签名镜像已生成；尚无这轮硬件执行结果。

比较过三个位置：页表刚写完时返回可以验证表内容，但不能验证 MMU；C 入口返回会额外涉及 NEON、栈访存与平台初始化；本轮选择 `MSR SCTLR_EL1` 后的 ISB 已执行、下一条指令尚未执行的位置。用一条 HVC 替换该指令，不叠加之前的入口检查点。

## 地址与执行契约

原有检查点在 MMU 关闭时只验证参数读取，其装载地址没有保留 kernel VA 的 16 KiB 块内偏移。2 MiB 块映射要求物理／虚拟地址的低 21 位一致。新模式将内核段整体移到 Image 自有区间内相同的偏移处，boot_args 使用对齐后的内存基址，内核自身再按 Mach header 位置调整参数；已验收的入口模式保持原样。

新模式使用独立的负载版本及附加元数据。除原有 Mach-O 审计外，打包器固定审计本次源码构建的完整冷启动前缀 SHA-256、set_mmu_ttb 的四条指令、页表池大小与范围、栈／异常向量符号、TCR 和禁止 Raspberry Pi 定时器 MMIO 的实际生成码。变化时拒绝生成镜像，不自动猜测新的代码路径。

已审计前缀只向 bootstrap page-table pool 写内存，修改 EL1 的栈、线程指针、翻译和异常寄存器；进入 C 之前不使用新栈进行访存。HVC 紧跟 MMU 开启后的 ISB，其前没有启用缓存后的数据写入，因此本轮回退不依赖清理未知脏缓存行。后续若移动检查点必须重新审计该假设。

## 返回与验收

EL2 使用自己的原有栈，捕获返回 PC、EL1t、SCTLR、参数寄存器、TCR、TTBR0/1、MAIR、SP_EL0/1 和 VBAR。保留 EL1 翻译开启状态时，分别对 boot_args 的高虚拟地址与检查点对应的高虚拟地址执行 AT S1E1R，保存 PAR_EL1 结果，然后关闭 EL1 M/C/I、使 TLB 失效。返回门通过后恢复跳入 XNU 前保存的 EL1 状态与原有 EL2 状态，再交接 Linux。

纯 C 校验器核对完整 64 KiB 页表池，包括未使用项应为零、根表／L2 分配顺序、描述符属性，以及 identity 和高地址到同一物理内存的映射。原始参数经过 Mach header 调整后的值、虚拟栈、异常向量和硬件 AT 结果必须一致，且全部实际用到的内核／参数数据位于已映射范围。

源码启动映射按剩余内存向下取整为 2 MiB 块，因此本轮只要求所有已使用数据被覆盖，不能宣称已建立完整最终 RAM 映射。检查点不进入 C、不读取 ADT、不初始化外设，不验证完整 XNU 或 iPadOS。

## 实施与验证

依次实现可选 MMU 负载、偏移装载、寄存器与 AT 捕获、纯 C 页表校验和日志读取门限。回归覆盖旧入口模式、偏移保留、错误元数据与越界、各寄存器／描述符／翻译结果的拒绝，以及真实生成码和容器布局。独立源码审查后生成签名镜像；外置介质写入继续使用私下固定目标与哈希的防护脚本，完整回读后再进行手动启动。


## 构建与读取

```sh
python3 tools/xnu_checkpoint.py --kernel /path/to/source-kernel \
  --kernel-report build/xnu-source/source-preparation.json \
  --adt build/xnu-handoff/kernel.macho.adt --output build/xnu-mmu/payload.bin --mmu
python3 tools/linux_log.py --source /path/to/native-linux.kpart \
  --futility /path/to/futility --key-dir /path/to/developer-keys \
  --xnu-checkpoint build/xnu-mmu/payload.bin --output build/xnu-mmu-usb
```

没有 `--mmu` 时继续生成已验收的旧入口模式；两种负载的原始源码内核一致，各自只替换一条指令。工具只生成普通文件。新模式回到 Linux 后用 `sudo python3 tools/read_probe.py --xnu-mmu`，同时要求原有八个路径／XNU 字段和三个唯一且值为 1 的 MMU 字段：

```text
XNU MMU enabled=0x0000000000000001
XNU tables verified=0x0000000000000001
XNU translations verified=0x0000000000000001
```

59 项主机回归通过，包括精确的 19,106,336 字节源码内核（60204 条本地重定位）与新旧模式装载、16 KiB 偏移、跨 L1 页表分配、完整未使用表项、寄存器／PAR 故障拒绝、生成码和包装 Image 布局。独立源码审查核对了实际前缀／helper／MMU 返回契约，无剩余阻断项。四种原有探针构建通过。

实际 Image 仍声明 74 MiB 内存，保留原生 Linux、47 份完整 DTB、initrd 和命令行。源码内核完整段跨度为 0x1310000；加上偏移、参数及本轮 ADT 均在 22 MiB 启动映射覆盖范围内。全部构建产物留在忽略目录，主机通过不等于本轮 MMU 已在真机开启。
