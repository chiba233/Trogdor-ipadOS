# XNU 源码与装载前准备

日期：2026-10-27。原生探针已经完成 EL2→EL1→EL2 的硬件验收；本阶段建立可复现的源码内核输入和主机装载检查，尚不跳入 XNU。

## 接口选择

沿用原厂 FIT shim。另两种选择是先接 UEFI／DarwinBoot，或移植 ios-sweet 的完整 shim；前者需要额外固件环境，后者绑定不同 SoC 与 XNU 版本。本阶段复用 UniXNU 的源码构建与 ADT 生成接口，后续再把产物接入已有 FIT 入口。

版本由 `upstream.lock.json` 固定：

- [UniXNU ae687009](https://github.com/UniXNU/UniXNU/tree/ae687009348296e97e49ce6e925795044f363ce6)：共享 ARM64／iPadOS profile、架构平台组件与构建入口。
- [Apple XNU 10063.141.1](https://github.com/apple-oss-distributions/xnu/tree/d8b80295118ef25ac3a784134bcf95cd8e88109f)：Darwin 23.6，源码和 ABI 依据。
- [DarwinBoot BootArgs.h](https://github.com/UniXNU/DarwinBoot/blob/3325e352d7df68a9f9ad74832ed94a6acbbd7390/DarwinBootPkg/Driver/BootArgs.h) 与实际装载代码：交接布局参考。

XNU 10063 的 ARM64 `boot_args` 使用 1024 字节 CommandLine，结构大小 0x480，deviceTreeP 位于 0x60、CommandLine 位于 0x6c、memSizeActual 位于 0x478。ios-sweet 的 XNU 8796 使用另一套 608 字节命令行 ABI，不能直接复制。内核为普通 ARM64，不能拿要求 PAC 的 ARM64e 镜像替代。

UniXNU 的该源码基线使用 4 KiB 页；内核编译配置中的 BCM2837 是上游复用的非 Apple ARM64 machine config 名称，不代表 Coachz 是 BCM2837。通用架构组件目前尚未在 Coachz 的 XNU 中验收。

## 源码构建入口

`platform/coachz` 只选择 ArmDTPlatform、ArmCpu、SoftwareAES 和 XnuPrelink。共享架构 patch series 按上游顺序解析，另显式选择两个 Darwin 23 prelink 接口补丁。OS profile 的可选 boot-progress 补丁依赖尚未接入的 UFS／owner-root 路径，因此本基线不应用该补丁，且保持 XNU_BOOT_PROGRESS=0。目前没有 Coachz 专用内核补丁。UFS、其他机型 Adreno、VirtIO 存储和 RAM block 均不作为已支持的 Coachz 外设。

```sh
python3 tools/sources.py fetch unixnu apple-xnu
python3 tools/kernel_source.py
python3 tools/kernel_source.py --build
```

默认只准备源码和构建树，`--build` 才运行源码编译。构建器仅初始化选中的四个组件，以及内核需要的七项开源 reference；不会初始化整套无关子模块，不调用 UEFI／固件构建，不自动下载 IPSW，也不执行 prelink。全部构建树、编译器记录与日志留在 `build/xnu-source`。报告将源码构建、prelink 和硬件启动分别标记。

macOS 构建需要完整 Xcode、CMake、Ninja 和 unifdef。CMake／Ninja 可单独安装在项目忽略目录，构建器会找到它们：

```sh
python3 -m pip install --target .tools/xnu-host-deps cmake==3.31.6 ninja==1.11.1.4
```

源码内核不是可直接交付的预链接 `kernel.macho`。后续仍需处理原始组件依赖、prelink、入口状态和日志接收。

## 内存与 ADT

`tools/xnu_prepare.py` 接收私有的完整原厂 FDT 和已校验的探针日志。它保留 RAM 区间的空洞，避让 FDT reservation 表、静态 reserved-memory、initrd 和原厂 FIT／FDT／Depthcharge 区域；动态 reserved-memory 的整个 alloc-ranges 都排除，没有边界则拒绝。

只从一个 RAM 区间选择 2 MiB 对齐的 256 MiB 候选窗口。该结果是主机规划，不是固件已分配的内存。真正装载前必须在当次启动重新验证并占有它，不能将随后 Linux 的内存快照当作跨启动分配凭证。

从日志中的 MPIDR 选择唯一启动 CPU，计时器频率使用实际 CNTFRQ。GIC 的寄存器通过其父总线 ranges 转换后，交给锁定的 UniXNU 公共 ADT 生成器。输出只包含单核、GIC 和架构平台的最小输入，以及公共生成器要求的 chosen、NVRAM proxy、随机种子、defaults、options 等；不虚构屏幕、存储、GPU 或触摸设备。它尚不含完整 iPadOS 产品身份和用户态配置。

```sh
python3 tools/xnu_prepare.py --dtb /path/to/private-live.dtb \
  --probe-log /path/to/private-probe-log.bin --output build/xnu-handoff
```

输出 `hardware.dtb`、`kernel.macho.adt` 和 `manifest.json`。每次生成新随机种子，哈希会变化。原始 FDT、日志、生成 ADT 和地址报告仅保存在忽略目录。

## Mach-O 与 boot_args 检查

提供 `--kernel /path/to/qualified-kernel.macho` 后，还会验证 raw ARM64-all MH_EXECUTE、load command 边界、段的文件／内存范围、虚拟段不重叠、4 字节对齐的入口位于可执行文件数据，并生成 `boot_args.bin`。

目前只接受已完成链接／重定位的输入；FAT、IMG4、ARM64e、LC_MAIN、dyld chained fixups、未解析符号、section relocations、外部 DYSYMTAB 重定位表和未知装载命令均拒绝。段标志只接受不改变放置的 SG_NORELOC／SG_READ_ONLY，SG_HIGHVM／受保护内容等明确拒绝。失败不能静默降级为继续启动。

对 LC_DYSYMTAB 的本地记录，只接受虚拟地址 slide=0、以首段为原点的 64 位非 PC-relative ARM64_RELOC_UNSIGNED。每个位置必须唯一、完整落在文件数据内，现有指针必须指向该记录指定的 section（包含合法的尾边界／空 section 边界）；不重写链接指针，不把物理装载偏移加到虚拟地址上。

ADT 指针按 kernel VA 填入 boot_args；x0 对应的是 boot_args PA。Video 保持零，避免假定原厂 splash 仍可用。数据按 16 KiB 对齐放在内核段后方，topOfKernelData 使用 PA，必须完整落在候选窗口内并保留尾部余量。

该工具不复制段到目标内存、不生成可启动镜像、不跳入内核。下一次真正的装载实验需要让 shim 在同次引导中执行这些检查，保留早期日志出口，再单独验证 XNU 入口。

## 验证边界

主机回归覆盖 Mach-O 截断／越界／重叠／错误 ABI、boot_args 字段偏移与 PA／VA 换算、原厂 FDT_END 特例、RAM 空洞和动态保留区、启动 CPU 与 GIC 总线转换，以及使用独立 ADT 解码器复核公共生成器输出。

真实私有 FDT 与已有寄存器日志已通过装载输入准备，成功生成最小 ADT。这是硬件数据上的主机检查，不是 XNU 已读取 ADT 的硬件结果。

## 已完成的主机验证

- 完整 41 项回归通过，包含直接对固定 Apple ARM64 boot.h 的 AArch64 编译期 ABI 检查和独立 ADT 解码。
- 真实私有 FDT／探针日志成功生成连续候选窗口、单核硬件 DTB 和 ADT；另以合成 Mach-O 跑通可选内核参数及 boot_args 文件生成。合成输入不代表真实内核。
- 独立审查发现并修复了 SG_HIGHVM 文件放置歧义；其余审查没有可操作问题。
- Xcode 27.1 的实际源码构建和最终链接成功，包含架构平台／CPU、软件 AES、corecrypto 和 pthread 源码组件。源码候选大小 19106336 字节，SHA-256 为 `6b406b8cd4ee5a6c11793256d8cadc5453365e8e8edb5f487953e02e259c7e9b`。
- 构建报告确认 `source_kernel_built=true`、`prelinked=false`、`hardware_boot_tested=false`。候选为 ARM64-all MH_EXECUTE，含 60204 条 LC_DYSYMTAB 本地重定位记录。后续逐条审计确认全部满足上述零 VA slide 契约，指针已由静态链接器填好；接受这些记录不代表完成组件 prelink。

已实现并生成原生源码 XNU 的 pre-MMU 检查点镜像，见 [入口检查点](xnu-checkpoint.md)。这只截取冷启动前缀；完整组件 prelink、页表／MMU 初始化、平台驱动与用户态仍待完成。
