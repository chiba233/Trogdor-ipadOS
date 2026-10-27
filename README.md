# Trogdor-ipadOS

HP Chromebook x2 11（Google Coachz / SC7180）的 **原生 iPadOS 移植实验**。

当前实现是第一阶段引导探针，已在真机取回寄存器日志，确认原厂 EL2 入口、EL1 诊断与 stage=3，并经 EL2 交接原生 Linux；源码 XNU 的 pre-MMU 入口检查点也已通过真机验证，确认在 EL1 读取 boot_args 后返回 Linux。目标用户态是已有社区原生实现可参考的 iPadOS 17；后续独立 MMU 检查点也已通过；高虚拟地址取指和 C 入口栈读取检查点也已通过；早期 C 正文的完整栈保存、参数复制与缓存清理返回也已真机验收，启动 CPU 数据的原始标量／FP/NEON 初始化也已通过；原始平台状态、ADT 绑定与首次原生 arm-io 查找已取回有效硬件日志；后续平台初始化、驱动与用户态接入仍待完成。

```text
原厂 Coreboot → TF-A BL31 → Depthcharge（NS EL2）
  → 开发者模式签名 FIT → ARM64 探针 → EL1 诊断
  → 后续：XNU Mach-O / Apple Device Tree / iPadOS
```

## 已有内容

- [原厂启动链研究](docs/native-boot.md)：固定到 Trogdor 固件维护分支，记录显示关闭、CPU 交接、内存与 FIT 格式的证据。
- [设计与阶段边界](docs/design.md)：比较直接 FIT shim、U-Boot EFI 与替换固件，明确首阶段的接口。
- `boot/`：带 Linux ARM64 Image 头的独立探针、EL2→EL1 入口、内存日志、受限 GENI UART 输出、只读 FDT 解析。
- `tools/`：交叉编译、FIT 封装、vboot 容器签名、上游版本核对；所有产物都是普通文件。
- [状态与真机验收](docs/status.md)：区分源码证据、主机检查、已通过的探针硬件验收与后续阶段。

## 构建

需要 Python 3、主机 C 编译器，以及 Zig 0.15.2。工具链可安装在项目内：

```sh
python3 -m pip install --target .tools/ziglang ziglang==0.15.2
python3 tools/build.py
python3 -m unittest discover -s tests -v
```

也可用环境变量 `ZIG` 指定已有的 Zig 可执行文件。

默认生成 memory、uart、reset-el2、reset-el1 四种探针：

| 产物 | 用途 |
| --- | --- |
| `build/memory/Image`、`probe.elf`、`probe.map` | 不访问外设的内存日志探针 |
| `build/uart/Image`、`probe.elf`、`probe.map` | 在原厂已经配置好 UART 的前提下，尝试输出日志 |
| `build/coachz-probe.dtb` | 仅供单核探针使用的最小 FIT 设备树；不是未来 XNU 的完整硬件描述 |
| `build/memory/probe.itb`、`build/uart/probe.itb` | 未签名 FIT；包含 Image 与探针设备树 |
| `build/manifest.json` | 实际使用的编译器、产物 SHA-256 与 Image 头信息 |

探针设备树的 `model` / `compatible` 是打包输入，不能当作真机身份探测结果。Depthcharge 会补入实际内存、原厂表位置等数据，探针记录这些交接信息。

构建对最终 ELF 的 AArch64 指令范围执行 [标量生成码检查](docs/codegen.md)，禁止早期探针依赖 FP／SIMD／SVE，并显式启用严格对齐。

## 无串口测试

见 [USB 测试步骤](docs/usb-test.md)。`reset-el2` 完成 EL2 诊断后等 3 秒复位；`reset-el1` 完成 EL1 诊断后等 10 秒复位。它们都不提供屏幕输出，观测到预期的成对延时重启只算粗粒度执行证据，不包含寄存器读数或 XNU 启动验证。

两种定时镜像观察到自动重启，普通内存对照与仅取消最终复位指令的匹配对照均不重启。单指令匹配对照为原版到达最终复位调用及此前 EL1／诊断门限提供间接硬件证据。后续 Linux 接收路线已取得直接寄存器日志；此前定时实验仍没有精确计时。见 [硬件记录](docs/hardware-tests.md)、[EL1 匹配对照](docs/control.md)与 [可读日志通道](docs/observability.md)。

在 Debian 构建／测试环境中，可使用已安装的 `futility` 和公共开发者测试密钥创建两个 64 MiB GPT 镜像：

```sh
python3 tools/prepare_usb.py
```

默认查找 `/usr/share/vboot/devkeys`、`/usr/share/vboot/devkeys-pkc` 和锁定的上游 `tests/devkeys`；也可用 `--key-dir` 指定。该命令只签名并创建文件，拒绝设备输出路径和覆盖已有文件。

## 签名

在具备 ChromiumOS `futility` 和开发者测试密钥的构建环境运行：

```sh
python3 tools/sign.py build/uart/probe.itb \
  --futility /path/to/futility --key-dir /path/to/devkeys \
  --output build/uart/probe.kpart
```

该命令仅创建并核验 vboot 容器，不写磁盘或固件。项目不提供自动刷写命令。将镜像放进可启动介质以及真机日志采集，是单独的硬件阶段。

## 当前可观测结果

探针在静态符号 `probe_log` 对应的内存中记录入口 EL、SCTLR、HCR、切换后的 EL、计时器频率、FDT 校验和交接属性；memory / uart 版本记录完成后停在 `WFE` 循环；定时复位版本在满足检查条件后请求重启。UART 版本只复用已初始化的 SC7180 QUPv3 UART，不下载固件，不改时钟或引脚；不可用时保留内存日志。

这一步不会恢复屏幕。原厂 Depthcharge 会停止 timing engine、关闭背光并复位 DSI，显示恢复需要独立验证。

## 原生 Linux 日志接收

已实现独立诊断路径：探针在 EL1 记录，受控 HVC 返回 EL2，交接保留的原生 Linux，在 chosen 设备树属性中读取日志。见 [构建与验收契约](docs/linux-log.md)。它使用现有原生 Linux 容器作为私有输入，不包含虚拟机，也不改变后续 iPadOS 引导目标；2026-10-27 已在真机启动并读回有效日志，确认 entry EL=2、current EL=1、stage=3、handoff EL=2。

## XNU 源码与装载前准备

下一阶段的源码入口、单核架构组件和主机装载检查见 [XNU 准备说明](docs/xnu-preparation.md)。`tools/kernel_source.py` 准备固定版本源码，`--build` 编译源码内核；`tools/xnu_prepare.py` 使用私有原厂 FDT／探针日志规划连续内存并生成最小 ADT，可选检查预链接 Mach-O 与 Darwin 23 boot_args。它们不生成新的可启动镜像，也不执行 XNU。

原生源码 XNU 的 pre-MMU 装载／boot_args 检查点见 [入口检查点](docs/xnu-checkpoint.md)。它已在真机通过单指令 HVC 返回 Linux 取回有效日志；后续 MMU、早期 C 参数复制、启动 CPU 数据及首次平台／ADT 查找检查点也已通过；后续平台初始化、平台驱动与 iPadOS 启动仍待完成。

[MMU 开启检查点](docs/xnu-mmu-checkpoint.md) 已通过真机日志验收：完整页表池、EL1 寄存器和硬件地址翻译校验成功，并返回 Linux。该轮只验证 MMU；随后高地址实际取指与栈读取由独立检查点确认。

[高虚拟地址与 C 入口检查点](docs/xnu-highva-checkpoint.md) 已通过真机验收：原始 RET 跳入 arm_init 高地址，完成栈帧调整与诊断读取后返回 Linux，未执行 C 初始化正文。

[早期 C 初始化与参数复制检查点](docs/xnu-cinit-checkpoint.md) 已通过真机日志验收：原始栈保存、完整参数复制、全局指针设置与缓存维护四个新字段及原有 11 字段全部通过，在 cpu_data_init 前返回 Linux；后续启动 CPU 数据及有限平台／ADT 查找的独立验收见下文。

[启动 CPU 数据初始化检查点](docs/xnu-cpu-checkpoint.md) 已通过真机验收：全部 16 字段有效，原始 cpu_data_init 的标量／FP/NEON 写入、完整 CPU 对象、保留哨兵与 guard 通过核对后返回 Linux。该有限检查点停在 PE_init_platform 前，不代表平台／ADT 初始化或完整系统启动。

[平台入口与原生 ADT 查找检查点](docs/xnu-platform-checkpoint.md) 已通过真机验收：完整 19 字段有效，正常 CPU 初值、原始平台状态与 ADT 绑定、原生 arm-io 查找及活动栈帧均通过核对后返回 Linux。检查点仍在后续 SoC 属性读取之前，后续平台／设备初始化与完整系统启动尚未验证。

[完整早期平台返回检查点](docs/xnu-pe-checkpoint.md) 已实现并通过主机回归与独立审查；首轮持续黑屏，尚未取得真机日志，诊断返回对照待验收。它合并 SoC 属性、CPU／时钟数据、型号与调试配置，并在完整 PE_init_platform(FALSE) 返回后停止；现有 ADT 缺失的总线／CPU 频率如实保持零，不代表时钟驱动已支持。
