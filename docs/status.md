# 状态与首阶段验收

更新：2026-10-27。

| 项目 | 状态 | 证据／限制 |
| --- | --- | --- |
| 原厂 Coreboot / BL31 / Depthcharge 调查 | 源码已核对 | 固定 Trogdor 维护分支，见 upstream.lock.json |
| NS EL2 入口与显示停止行为 | 源码已核对 | 尚未在目标设备确认实际固件行为 |
| ARM64 内存日志探针 | 已交叉编译 | ELF、Image、FIT；不复位对照已观察到保持黑屏、不自动重启；执行位置待确认 |
| 可选 GENI UART 探针 | 已交叉编译 | 仅复用已初始化 UART；未真机启动 |
| Image 头、ELF 符号与 FIT 内嵌数据 | 主机检查通过 | build/manifest.json；不能证明 CPU 指令在硬件执行成功 |
| 启动期生成码 | 主机回归通过 | 显式关闭 FP/NEON、启用严格对齐；ELF 指令范围检查通过；修复后匹配对照支持 EL1 诊断路径的间接执行证据，尚无直接日志 |
| 首次 EL2 USB 尝试 | 未通过 | 黑屏，未观察到预期复位；尚不能确认入口或停止位置 |
| FDT 解析与原厂 END token 格式兼容 | 主机测试通过 | tests/test_artifacts.py；包含截断和非法边界 |
| vboot 签名与容器验证 | 主机验证通过 | 使用锁定 vboot 分支构建的 futility / 公开测试密钥；两个检查点容器签名与 body 验证通过，篡改被拒绝；原生探针的执行获得间接支持，入口寄存器仍未读取 |
| 无串口 EL2 / EL1 复位检查点 | EL1 诊断路径有间接硬件证据 | 原版重启，最终 SMC 改为 NOP 的匹配对照不重启；支持此前 EL1／stage／FDT 门限；没有直接寄存器日志或精确计时 |
| GPT 镜像生成 | 主机测试通过 | 主／备 GPT CRC、分区类型、负载布局、文件保护；官方 vboot 对 64 MiB 镜像与模拟 8 GiB 介质的主表校验通过；匹配对照间接支持外置负载执行，完整设备验收待完成 |
| 仅抑制最终复位的 EL1 匹配对照 | 已观察到不自动重启 | Image 仅改一条 SMC 为 NOP，其余字节相同；与原版形成硬件对照，见 docs/control.md |
| 原生 Linux 日志接收 | 已构建签名，硬件待测 | EL1 探针→受控 HVC→EL2 Linux；29 项测试通过，保留完整 DTB／initrd／命令行，见 docs/linux-log.md |
| 显示恢复 | 未实现 | 原厂停止 timing engine / DSI，不能直接沿用 splash |
| XNU 装载、ADT、boot_args | 未实现 | 后续接入阶段 |
| eMMC、触摸、GPU 与完整 iPadOS 用户态 | 未实现 | 没有原生桌面或应用运行记录 |

## 已执行的主机验证

- Zig 0.15.2 的后端特性覆盖问题已定位，详见 docs/codegen.md；原有 17 项主机测试通过，四种 ELF 的指令范围通过标量检查。
- Zig 0.15.2 构建 memory / uart / reset-el2 / reset-el1 四种 AArch64 freestanding ELF。
- 从 PT_LOAD 段导出原始 Image，校验 ARM64 magic、跳转、内存大小和 ELF 符号。
- 生成并核对 FIT 的 Image / DTB 原始字节与 SHA-256。
- 运行 FDT 和产物测试；本地原厂参考源码的 HEAD 与锁定版本一致。
- 两个检查点已实际签名；核对签名容器／GPT 中的 FIT 原始字节一致，篡改 body 会被 futility 拒绝。
- 定时复位条件拒绝错误 EL、未完成阶段、不可用 PSCI 和异常计时频率；GPT 格式与仅文件输出限制通过测试。
- 独立源码审查发现原厂 dt_flatten() 的 structure_size 不计 FDT_END；已增加精确边界兼容与回归用例。

匹配对照增加 4 项回归，合计 21 项通过。实际产物检查确认除最终复位的四个指令字节外 Image 完全相同，FIT 只改变 kernel data 与其哈希。

## 硬件尝试与待完成验收

已记录的尝试见 [硬件记录](hardware-tests.md)。仅凭最初两轮自动重启不能确定执行位置；后续单指令匹配对照不再自动重启，使最终 SYSTEM_RESET 及此前 EL1／诊断门限获得较强的间接硬件支持。这不是直接寄存器测量，尚未取得完整探针日志。下一步是建立可读输出，见 [可观测性调查](observability.md)。

以下验收尚未完成：

1. 使用已完成主机签名验证的镜像，并核对交付包 SHA-256。
2. 先按 docs/usb-test.md 执行无串口复位检查点，或建立 AP UART / 内存调试通道。复位检查点不提供日志，内存日志也不是复位后仍可靠的持久日志。
3. 从单独准备的可启动介质，经原厂开发者模式进入探针。
4. 记录入口 EL、当前 EL、MIDR / MPIDR / CNTFRQ、FDT 内存与 Coreboot 表位置。
5. 验收 current EL = 1 且 stage = 3；如异常，保存 ESR / ELR / FAR 和探针符号版本。
6. 原厂显示恢复另立验证步骤，然后再确定 XNU 内存布局与 ADT。

没有完成以上设备验收前，不将主机构建或单元测试记为“原生启动成功”。
