# USB 只读实验

`tools/linux_log.py --usb-read` 生成独立镜像，不启动 XNU，只回答一个问题：探针能否通过 xHCI 和集线器枚举启动介质，并用 DMA 读到它的真实内容。全程只发读命令，读完停掉控制器，然后沿已验证的路线回到 Linux 取日志。验收用 `tools/read_probe.py --usb-read`。

USB 栈是 Depthcharge 启动 U 盘时用的同一套 libpayload 代码（BSD）。源码不进仓库，`tools/usb_stack.py` 构建时从固定的 `upstream/coreboot` commit 取出，`boot/usb_compat/` 是很薄的兼容头。构建时做两处改动：

- 函数指针表改为运行时赋值，因为包装器链接在 0 地址，运行时不做重定位。
- xHCI 寄存器结构体去掉 `__packed`。探针用 `strict_align` 编译，对 packed 成员的访问会被拆成单字节，MMIO 寄存器不接受。第一次实验的 `DCBAAP` 因此读回错误地址，控制器报 HSE 和 HCE。`boot/usb_read.c` 用静态断言锁定寄存器偏移。

判断方法：

| 问题 | 看什么 |
| --- | --- |
| CPU 能否访问控制器 | `CAPLENGTH`、`HCSPARAMS1` |
| SMMU 有没有拦截 DMA | `SMMU sCR0` 的 `CLIENTPD`，`SMMU sGFSR` |
| 控制器有没有报错 | `USBSTS` 的 HSE、HCE |
| DMA 读到的是否真实 | `lba0 tail` 的 `55AA`，`lba1 head` 的 `EFI PART`，`lba2048 head` 的 `CHROMEOS` |

Coachz 上的事实：控制器在 `0x0a600000`，中断是 GIC INTID 165（SPI 133），SMMU stream 是 `0x540`。固件交接后 SMMU 处于旁路状态。启动介质走 USB3，挂在板载 Realtek 集线器上，键盘 Hammer 挂在同一个集线器的 USB2 一侧。这些都是探针或 Debian 在真机上读到的。

这个实验没有用到中断，也没有开 MMU 和缓存。XNU 里的驱动要自己解决 Device 映射、非缓存 DMA 内存和块设备接口。

XNU 里的驱动在 `platform/coachz-prelink/drivers/RamBlock`。控制器用 `ml_io_map` 映射成 Device 内存，DMA 池是 XNU 物理窗口外的一段固定物理内存，用 `ml_io_map_wcomb` 映射成非缓存内存。两者都从 ARM 固定 8 MiB 的 `io_submap` 分配，所以 DMA 池只有 1 MiB；16 MiB 的第一版在映射时就 panic 了。2026-10-31 真机屏幕确认内核读到了同样三个扇区。
