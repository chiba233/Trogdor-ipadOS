# 源码 XNU 入口检查点

目标是在原厂签名 FIT 路径原生执行源码内核的冷启动前缀，验证实际读取的 boot_args，再回到已有 Linux 接收日志。当前只有主机验证，尚无这轮真机结果。

## 执行边界

输入为锁定 XNU 10063.141.1 的源码构建产物；没有完整组件 prelink。打包器验证 LC_UNIXTHREAD 入口直接跳转到 `_start_first_cpu`，核对其前 13 条指令和 bootstrap page-table 符号。仅将读取 boot_args 后的 `MSR TPIDRRO_EL0,xzr` 四字节指令替换为 HVC #0x584e。原始 60204 条本地链接指针逐条审计，VA slide 保持零，指针内容不变。

这段真实 XNU 代码设置异常向量并读取 virtBase、physBase、memSize 和 bootFlags，在写页表、启用 MMU、更换栈或执行 C 初始化之前返回。它不验证 ADT 消费、驱动初始化、完整 XNU 启动或 iPadOS 用户态。

包装 Image 同时包含原生 Linux 和内核负载，并在 image_size 中声明 24 MiB、2 MiB 对齐的独立 XNU 区间。NOLOAD 区间不扩大镜像文件。实际包装 Image 申请 74 MiB，可落入原厂可用的单一连续区间；不能将主机规划地址当作本次启动的固件分配。

装载前从当次 FDT 重验 RAM、FDT 自身、reservation 表、initrd、Coreboot／CBMEM、静态 reserved-memory、动态 alloc-ranges 和原厂引导器区域。无法解释的布局拒绝装载。所有负载边界和段布局验证完成后才清零并复制；boot_args 与 ADT 的 DRAM 字段使用本次 Image 内实际地址，Video 仍为零。

EL2 返回门检查精确 PC、EL1h、SCTLR_EL1 的 M/C/I 状态，以及 XNU 保存的 boot_args 指针和读取值、页表物理地址。全部匹配后恢复保存的 EL2 控制寄存器，并交接原生 Linux。装载失败可以返回 Linux 输出 `XNU copied=0`；其他异常停止，不能当作成功。原有 HVC #0x5452 仅允许包装器内的精确失败返回 PC。

## 构建与读取

```sh
python3 tools/xnu_checkpoint.py --kernel /path/to/source-kernel \
  --kernel-report build/xnu-source/source-preparation.json \
  --adt build/xnu-handoff/kernel.macho.adt --output build/xnu-checkpoint/payload.bin
python3 tools/linux_log.py --source /path/to/native-linux.kpart \
  --futility /path/to/futility --key-dir /path/to/developer-keys \
  --xnu-checkpoint build/xnu-checkpoint/payload.bin --output build/xnu-checkpoint-usb
```

工具仅生成普通文件，不自动写设备或重启。FIT 保留原 Linux、initrd、47 份完整 DTB 和命令行，只加入检查点包装器与日志属性。

启动回到 Linux 后：

```sh
sudo python3 tools/read_probe.py --xnu-checkpoint
```

该模式同时要求原生 EL2→EL1→EL2 路径，以及 `XNU copied=1`、`XNU checkpoint=1`、`XNU boot_args verified=1`、`XNU current EL=1`，每个字段必须唯一。普通 Linux 能启动本身不证明 XNU 检查点成功。

## 主机验证

51 项回归通过，包含合成 Mach-O 的真实指令前缀、零 slide 指针、错误重定位拒绝、段复制与 BSS 清零、boot_args 与 ADT 地址填充、失败前零写入、运行时保留内存排除、逐寄存器／MMU 返回校验、日志重复／缺失字段拒绝，以及 AArch64 标量生成码和 Image 内存布局检查。四种原有探针构建通过。

实际源码负载与签名容器已生成并验证；主机结果不等于硬件执行结果。后续真机观察记录在 hardware-tests.md。
