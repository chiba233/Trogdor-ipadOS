# XNU C 初始化与参数复制检查点

沿用直接 FIT → 源码 XNU → 受控 HVC → 原生 Linux 的既定路线。下一次检查点保留真实 arm_init 序言和最早的 C 正文，执行完整 boot_args 复制与 BootArgs 全局指针设置，在 cpu_data_init 之前返回。实现、74 项主机回归与独立审查已通过；外置签名镜像已写入并全量回读，C 正文真机执行仍待验收。

比较过仅放行寄存器保存、直接放行 PE_init_platform 和在参数复制后停止。选择参数复制后停止：它同时覆盖原始栈写入、memmove 的子调用栈帧和全局写入，边界明确；平台初始化与 ADT 消费留待下一轮单独验收。

## 执行范围

version=4 从原始源码内核独立打补丁，不叠加此前三种检查点。固定完整输入内核 SHA-256、冷启动前缀、arm_init 前缀和两个被调用函数的生成码。该内核的 arm_set_kernel_tbi 是 RET；普通 Mach-O 头不进入 fileset 分支，thread_starts 的两个已链接地址相等，因而不进入重定位循环。进入原始冷启动前仅对本模式将 x27/x28/x29 归零，冷启动路径保留它们；x19 被原始 TTBR 调用序列设置为 C 入口地址，使整个寄存器保存帧都有独立期望值。实际路径把 static_kernelcache 与 vm_kernel_slide 写为零，调用原始标量 memmove 复制 0x480 字节，并写入 BootArgs。仅把随后的 ADRP 替换成 HVC；不进入 cpu_data_init。

装载器在任何写入前验证参数复制目标、BootArgs、slide、static_kernelcache、stack guard 和含 memmove 子帧的 0xe0 字节栈范围，均属于 Image 自有内核非执行段，互不重叠且与完整页表池分离；初始值必须符合已审计输入。旧模式保持原有契约。

## 返回与验收

源代码会改写 x19/x20，不能继续把 x20 当作原始参数 PA。EL2 的 AT 输入和后续日志使用装载前保留的预期参数，实际寄存器按本模式独立检查。仍要求原有 M/C/I、完整 64 KiB 表池、参数／代码／栈的 AT 和 EL1 现场检查。

真实栈和全局写入可能留下脏缓存。EL2 MMU 关闭，以 Image 自有 24 MiB arena 的物理值作为维护 VA，按 CTR_EL0.DminLine 遍历全部缓存行，前后 DSB SY，再关闭 EL1 M/C/I。不得从未验证的 C 寄存器生成维护区间，也不清理 arena 之外的地址。随后按物理地址核对完整参数复制、BootArgs 指针、两个 slide 全局、完整 0xe0 字节的原始寄存器保存／局部变量／memmove 子帧痕迹；全部通过才恢复现场并交接 Linux。

严格读取模式新增 C 正文、栈写入、全局参数复制和缓存维护四个字段，必须同时满足此前的路径／XNU／MMU 门限。不得沿用旧版诊断 LDR 的零值门限，或仅因 Linux 返回而认定通过。硬件验收前只报告源码审计、主机测试、生成码与签名容器验证结果，不声称 C 正文或 iPadOS 已在真机执行。

## 验证计划

覆盖原有三种模式、精确源码输入和补丁范围、未审计代码拒绝、非执行段／初始值／重叠的写入前拒绝、各现场字段与完整复制内容的破坏拒绝、栈保存和子调用帧的逐项拒绝、严格日志门限，以及实际交叉编译后的标量与缓存维护顺序检查。独立审查后生成签名普通文件；外置测试介质继续属性检查、固定哈希与全量回读，不自动重启。

## 构建与读取

```sh
python3 tools/xnu_checkpoint.py --kernel /path/to/source-kernel \
  --kernel-report build/xnu-source/source-preparation.json \
  --adt /path/to/generated.adt --output build/xnu-cinit/payload.bin --cinit
python3 tools/linux_log.py --source /path/to/native-linux.kpart \
  --futility /path/to/futility --key-dir /path/to/developer-keys \
  --xnu-checkpoint build/xnu-cinit/payload.bin --output build/xnu-cinit-usb
```

工具只生成普通文件。成功返回 Linux 后，以 `sudo python3 tools/read_probe.py --xnu-cinit` 要求原有路径／XNU／MMU 的 11 个字段，加上四个唯一且值为 1 的新字段：

```text
XNU C body verified=0x0000000000000001
XNU stack writes verified=0x0000000000000001
XNU args copy verified=0x0000000000000001
XNU cache maintenance verified=0x0000000000000001
```

旧高 VA 模式的诊断栈读已被原始代码取代，因此本模式使用新的完整数据／栈验收门限。全部 15 个严格字段通过后，还须独立比对本轮精确返回 PC、参数地址换算与负载清单。复制校验覆盖全部 0x480 字节，栈校验覆盖父／子帧全部 0xe0 字节；每个字节分别破坏的主机测试均会拒绝。

## 主机验证与介质准备

74 项主机回归通过，四种原有探针可构建。原始输入内核 SHA-256 为 `6b406b8cd4ee5a6c11793256d8cadc5453365e8e8edb5f487953e02e259c7e9b`；只修改 C 复制后的单条四字节指令，真实反汇编确认原始 memmove 与 BootArgs 写入保留，随后的 HVC 位于 cpu_data_init 之前。独立审查修正了初版仅检查部分栈槽的问题，最终复核完整帧、现场恢复、缓存维护与生成码，没有剩余阻断项。

| 产物 | SHA-256 |
| --- | --- |
| instrumented C-init kernel | 52a85daebb92af7163406e808cfe9e7e4bc92e2ce1e89e1146df0b51812fe061 |
| checkpoint payload | ba4ded33bb3ddc43c1649335ee9c6e805dd1ace21252868704a5cc3253f13eef |
| linux-log.kpart | ee29ea075c3d6bbeed686c834c8e70058042a7c4309356380cfcb8a7367fbd23 |
| linux-log.img | 5ce7714eacccd5e527b9b1d6e0d4ee05626e83ce1f0c069f617c9de9227ab119 |

包装 Image 文件与内存申请分别仍为 51444224／77594624 字节；Linux、47 份完整 DTB、initrd、命令行保持不变。签名／body、实际主备 GPT CRC 和内嵌容器检查通过，目标端传输哈希一致；外置介质完整 128 MiB 回读哈希匹配。没有写入固件或内置盘，没有自动重启。完整源码二进制、ADT 和原始设备记录留在忽略目录。

缓存操作语义参考 [Arm DC CIVAC](https://support.arm.com/documentation/ddi0601/2026-09/AArch64-Instructions/DC-CIVAC--Data-or-unified-Cache-line-Clean-and-Invalidate-by-VA-to-PoC?lang=en)：按 VA 清理并失效到 PoC，地址可能需要翻译。本轮实际维护区间由 EL2 的链接符号给出，EL2 MMU 关闭；生成码确认整个有界循环及其前后 DSB 位于关闭 EL1 M/C/I 之前。此为主机生成码证据，尚不能代替真机脏缓存返回验收。

当前待手动从原厂开发者模式启动并取得新的 Linux 日志；仅返回 Linux、黑屏或重启都不能单独证明 C 初始化成功。此模式不消费 ADT，不进入平台初始化，不代表完整 XNU 或 iPadOS 启动。
