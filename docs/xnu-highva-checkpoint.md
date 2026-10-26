# XNU 高虚拟地址与 C 入口检查点

沿用已通过的 MMU-on 路线，目标是证明源码启动路径实际跳入高虚拟地址，并能使用该地址空间中的栈。实现、66 项主机回归与独立审查已通过，签名镜像已生成；尚无本轮硬件执行结果。

比较过在启动汇编中插入自制高地址跳板、直接运行 C 正文、以及在原始 arm_init 入口设检查点。选择最后一种：保留源码启动汇编直到 RET，保留 arm_init 第一条 SUB SP 的原始栈帧调整；把接下来的两条 STP 分别换成 LDR x19,[sp,#0x70] 和 HVC。这是到达真实 C 函数地址与读取栈的检查点，不执行 C 初始化正文，也不验证栈写入。

## 执行与输入契约

负载 version=3 保留 mode2 的物理／虚拟块内偏移。打包器额外固定从冷启动入口到 RET 的完整前缀哈希，核对私有外部符号 arm_init、生成 link register 的 ADRP/ADD 目标和原始三条函数序言。旧入口和 MMU 模式保持独立，各自在原始源码内核上打补丁，不叠加检查点。

完整前缀会启用 CPACR_EL1.FPEN、清零 TPIDR_EL1，将 x0 转成 boot_args KVA，再 RET 到 arm_init。第一条原始 SUB 将 SP_EL0 降低 0xd0，诊断 LDR 从新 SP+0x70（原栈顶-0x60）读取八字节零值。打包器和装载器都验证栈帧／槽位属于内核非执行段、与页表池不重叠，且加载后的槽位为零。

检查点之前仍没有缓存开启后的数据写入。只读栈加载可能留下干净缓存行，EL2 捕获后以对应物理地址执行 DC CIVAC 和 DSB，地址必须落在 Image 自有区间内；随后沿已验收路径关闭 EL1 M/C/I 并恢复状态。新增保存和恢复 CPACR_EL1、TPIDR_EL1，不扩展到驱动或外设。

## 验收门限

在原有 MMU／完整页表／硬件 AT 校验之外，要求：ELR 是精确的 arm_init 高地址检查点之后；SP_EL0 为预期栈帧地址；x0 为 boot_args KVA；LR 为原始 arm_init KVA；诊断 LDR 的 x19 为零；CPACR_EL1.FPEN、TPIDR_EL1 值正确。硬件 AT 除参数和代码外还核对实际栈槽。高地址 ELR 直接送入 AT，旧 identity 模式仍先转换为 KVA。

校验通过后恢复 EL1／EL2 状态并返回原生 Linux。新读取模式必须同时要求原有字段和高地址／栈读取／C 入口三个新字段，不能仅靠 Linux 回来判定成功。错误返回仍停止。

## 验证计划

覆盖 mode1/2 兼容、精确原始源码与合成输入、补丁范围、前缀／符号／序言错误拒绝、栈槽非零／范围／页表重叠、失败前零写入、高地址 PC 与参数／栈／LR／系统寄存器／PAR 各项拒绝、读取器字段门限、交叉编译及标量生成码。独立审查后生成签名镜像，外置 U 盘继续固定哈希和设备属性并完整回读，不自动重启。


## 构建与读取

```sh
python3 tools/xnu_checkpoint.py --kernel /path/to/source-kernel \
  --kernel-report build/xnu-source/source-preparation.json \
  --adt build/xnu-handoff/kernel.macho.adt --output build/xnu-highva/payload.bin --highva
python3 tools/linux_log.py --source /path/to/native-linux.kpart \
  --futility /path/to/futility --key-dir /path/to/developer-keys \
  --xnu-checkpoint build/xnu-highva/payload.bin --output build/xnu-highva-usb
```

工具只生成普通文件。回到 Linux 后，用 `sudo python3 tools/read_probe.py --xnu-highva`，它要求此前全部路径／XNU／MMU 字段，以及三个唯一且值为 1 的新字段：

```text
XNU high VA verified=0x0000000000000001
XNU stack read verified=0x0000000000000001
XNU C entry verified=0x0000000000000001
```

66 项回归通过，包含精确源码内核的三种负载装载、私有符号与 STAB 区分、补丁范围、序言／返回前缀拒绝、错误栈／非零槽位的写入前拒绝、24 项现场字段逐一拒绝、日志字段缺失／重复和实际交叉编译。四种原有探针仍可构建。独立审查复核实际生成码、物理缓存维护 VA、现场保存恢复及全部 XNU 回归，未发现剩余阻断项。

包装 Image 仍声明 74 MiB，保留 Linux、47 份完整 DTB、initrd 和命令行。完整源码内核、负载、ADT 与原始设备数据均留在忽略目录。当前未证明高地址取指、栈读取或 C 入口已经在硬件执行；更不代表 C 初始化正文或 iPadOS 已运行。

## 缓存维护依据

[Arm DC CIVAC 指令说明](https://support.arm.com/documentation/ddi0601/2026-09/AArch64-Instructions/DC-CIVAC--Data-or-unified-Cache-line-Clean-and-Invalidate-by-VA-to-PoC?lang=en) 定义按 VA 清理并失效到 PoC，该 VA 可能需要地址翻译。本设计固定 EL2 MMU 关闭，因而维护操作使用栈槽对应的物理值作为 EL2 VA；它与保留 EL1 翻译来执行 AT 的地址空间不同。地址由捕获上下文换算，并限制在 Image 自有区间内。该契约经源码审查，仍待本轮真机验证。
