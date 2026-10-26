# Trogdor-ipadOS

HP Chromebook x2 11（Google Coachz / SC7180）的 **原生 iPadOS 移植实验**。

当前实现是第一阶段引导探针，尚未接入 XNU，尚未确认探针在真机执行成功。目标用户态是已有社区原生实现可参考的 iPadOS 17；具体 XNU 构建与驱动接入在后续阶段进行。

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
- [状态与真机验收](docs/status.md)：区分源码证据、主机检查与尚未通过的硬件验收。

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

目前两种镜像都观察到自动重启，但尚未观察到可辨认的延时差异，不能确认通过检查点。见 [硬件尝试记录与不复位对照](docs/hardware-tests.md)。

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
