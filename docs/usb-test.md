# 无串口 USB 原生检查点

这是独立 ARM64 诊断代码，不含 Linux、XNU 或 iPadOS。已有黑屏和自动重启的硬件尝试，但尚未确认检查点执行成功，见 [硬件尝试记录](hardware-tests.md)。沿用原厂开发者模式与 Depthcharge 的外置介质启动入口，不替换 SPI 固件。

## 已生成的镜像包

若使用已经生成的 `coachz-checkpoint-usb.tar.gz`，直接解压并检查 SHA-256，无需安装编译器或重新签名：

```sh
tar -xzf coachz-checkpoint-usb.tar.gz
cd coachz-checkpoint-usb
sha256sum -c SHA256SUMS
```

镜像包仅含项目诊断代码、已签名容器、普通 GPT 镜像及校验信息，不含固件转储或 Apple 软件。发布前的 futility 签名／body 校验和 GPT 主机检查不能代替设备验收。

## 从构建产物准备镜像文件

构建环境需要 Python 3.9 或更新版本、`futility` 与公共开发者测试密钥。Debian 的 `vboot-kernel-utils` 提供签名工具。运行：

```sh
python3 tools/prepare_usb.py
```

如果没有找到密钥，可从锁定的官方 vboot 源码获取：

```sh
python3 tools/sources.py fetch vboot
python3 tools/prepare_usb.py --key-dir upstream/vboot/tests/devkeys
```

输出在 `build/usb/`：

| 文件 | 行为 |
| --- | --- |
| reset-el2.kpart / reset-el2.img | 留在 EL2，FDT 与诊断检查通过后等 3 秒请求复位 |
| reset-el1.kpart / reset-el1.img | 切换到 EL1，FDT 与诊断检查通过后等 10 秒请求复位 |

`.kpart` 只含已签名内核容器；`.img` 才是带主／备 GPT 的完整介质镜像。每个镜像 64 MiB，包含一个 16 MiB ChromeOS kernel 分区，priority=1、tries=1、successful=0。固件每次启动都会用掉一次 tries，用完就把分区标记为坏分区，所以同一次写入只能启动一次，要再启动就得重写 U 盘。例外是 `linux_log.py --xnu-screen`，它进入 XNU 后不再返回 Linux，因此设为 tries=15。制作镜像的程序不会打开或改写磁盘设备。重复生成时使用新的 `--output` 目录，避免覆盖已有文件。

## 写盘前确认

需要一只可清空且容量至少 64 MiB 的 U 盘。写入镜像会覆盖目标介质的分区表与内容，应先确认物理设备名、容量、USB 连接方式和已有挂载；不要把 `.kpart` 当成完整磁盘镜像。项目不自动选择目标盘，也不自动执行写盘。

在 Debian 上列出磁盘：

```sh
lsblk -d -o NAME,SIZE,MODEL,TRAN
lsblk -o NAME,SIZE,TYPE,FSTYPE,MOUNTPOINTS
```

确认目标后再手动写入一个 `.img`，卸载／安全移除后重新插入。小镜像写入更大介质时，备份 GPT 位于镜像的 64 MiB 边界，部分磁盘工具会提示备份表不在物理介质末尾；这不是要求修改内置存储。锁定分支的 vboot `GptValidityCheck` 已在 64 MiB 文件布局和模拟 8 GiB 物理介质布局上验证主表可用。主／备 GPT 在生成的镜像文件内均有 CRC 校验。

## 执行与记录

1. 先制作并启动 `reset-el2.img`。从原厂开发者菜单选择外置介质启动；从屏幕关闭开始计时，观察是否在约 3 秒后发生实际重新开机。
2. 重新制作 U 盘，换为 `reset-el1.img`，用同样的方法观察约 10 秒后的实际重新开机。
3. 记录镜像 SHA-256、两次观察时间、是否出现固件错误，以及是否真正重新开机。镜像加载耗时和屏幕关闭到探针入口之间的时间会带来偏差。
4. 若两次都出现与 3 / 10 秒相符且相差约 7 秒的重启，可以记为相应路径到达定时复位检查点的初步证据。仍不能报告寄存器值、存储或显示驱动已验证，更不能报告 iPadOS 启动成功。

只有在 FDT 合法、诊断 stage=3、实际 EL 符合目标、PSCI 至少 0.2、计时频率有效时才会等待并请求复位。不满足条件、异常或复位调用意外返回时会停止。一直黑屏不是成功；固件拒绝镜像后回到菜单不是重启；手动复位和不可解释的自动重启也不算通过。必要时用电源键恢复正常开机，随后从内置存储启动。

默认启动目标取决于现有固件设置；测试时手动选择一次外置介质，不把它设置为默认启动。发生意外重复启动时拔掉测试 U 盘。

## 延时差异不明确时的对照

使用既有 `memory` 探针作不复位对照。它沿用 EL2→EL1 和 FDT 诊断路径，返回后停止在 WFE，不调用定时检查点。该镜像不会主动请求重启；若同样自动重启，必须继续调查其他复位来源，不能把重启直接记为诊断通过。黑屏保持也不能证明 EL1 或 FDT 诊断成功。

构建后可创建普通镜像文件：

```sh
python3 tools/sign.py build/memory/probe.itb \
  --futility /path/to/futility --key-dir /path/to/devkeys \
  --output build/halt/memory.kpart
python3 tools/disk.py build/halt/memory.kpart build/halt/memory.img --label memory-el1-halt
```

显式指定外部 `futility` 和公开开发者测试密钥目录，替换上面两处路径；该签名命令没有自动查找选项。按前述写盘前确认步骤制作测试介质；从选择外置启动开始观察 30 秒，记录是否自动重启，随后可用电源键恢复并从内置存储启动。

若需保留 PSCI_VERSION 和等待过程、只取消最终复位调用，使用 [EL1 匹配对照](control.md)。它与普通 memory 版本是不同的诊断产物，不能混用各自的观察结论。
