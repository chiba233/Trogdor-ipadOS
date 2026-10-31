# SC7180 TSENS 只读温度采样（待接入）

这里是可供接手者接入的源码，**不在活动内核构建中**。没有编译 XNU、打包镜像、写盘或实机验收。

- `coachz_tsens.h`：可移植的只读采样和换算；两组传感器分别使用 `0x7fff`、`0x03ff` 掩码。
- `coachz_thermal.c`：XNU MMIO 映射、启动参数和 ADT 校验、诊断输出。
- `coachz-prelink-thermal.patch`：加入这两个文件和 sampler 调用点的独立候选补丁；未加入 `series`。
- `adt.py`：使用现有装载器 `Node` 的可选节点生成函数；活动装载器未引用它。
- `SOURCES.md`、`LICENSE`：固定版本证据、移植差异和保留的上游许可证。

## 行为

同时具备 `coachz-thermal=1` 和明确的 `/arm-io/coachz-thermal` 节点才映射寄存器。节点的 `reg` 是四组**物理地址**与大小，不是通用 `arm-io` 相对偏移；驱动逐项校验固定地址后才映射。范围覆盖 TM0、SROT0、TM1、SROT1，每组窗口 `0x200` 字节（DTS 的 `0x1ff` 长度向上取整，实际只读已列出的对齐寄存器）。

检查顺序：版本主号 2 → controller enable → 不在 reset → sensor enable → TRDY bit 0 → 单路 VALID bit 21。温度是有符号 12 位的 0.1°C，乘 100 得到 m°C。无效数据最多连续读三次，不等待控制器启动，不使用上一轮温度补值。

第一次输出两组寄存器摘要和各路状态／温度；之后在现有 sampler 的约 10 秒唤醒点输出每组有效掩码和有效通道最高温度。轮询放在诊断的 `continue` 分支之前，避免其他报告吞掉采样。两个入口都由同一个 sampler 线程调用，没有新增线程或并发消费者。

它不写 TSENS、时钟、复位、校准、阈值、中断、PMIC 或 CPU 调频寄存器。各 bank 的最高温度只是当前有效通道的最大值，未宣称是完整 CPU 温度，也未接入 iPadOS thermal 服务。`no-valid-temperature` 是不可用，不是 0°C。

## 接入交接

1. 在接手者决定的构建阶段应用候选补丁。补丁以已有 `coachz_touch.c`、`coachz_ec.c` 和 `coachz_sample.c` 为上下文，不依赖待接入 Wi-Fi；与 GPU 的启动钩子可共存。不要重复应用。
2. 在装载器写出／测量 ADT 之前调用 `adt.py` 中的 `add_thermal_sampler(root)`，再决定是否给命令行加入 `coachz-thermal=1`。辅助函数不能在 ADT 已序列化后补调用。
3. 由接手者编译内核和准备镜像；此目录不会自动触发这些操作。
4. 从第一轮实机日志确认版本、控制寄存器、ready、有效掩码与每路值。若是 disabled、not-ready 或 unsupported-version，保留原始日志并调查固件状态；不要顺手写入其他 SoC 的初始化值。
5. 再做连续多轮变化与 Debian 同 bank／sensor 的温度对照。读到合理的单个数值不等于校准、完整温控或休眠恢复已经验证。

`ml_io_map()` 使用现有 XNU 的 IO 映射属性，映射保留至内核结束；这里不提供可卸载或深睡恢复接口。映射返回地址不代表总线读取必定成功，真实 MMIO 异常仍会走内核异常路径，主机回调测试不构成异常恢复能力。

## 主机检查

在仓库根目录运行：

```sh
python3 -B -m unittest discover -s tests -p test_thermal.py -v
```

只编译独立主机 C 模型，并检查实际 `Node` 的序列化、候选补丁应用和活动构建未启用。另已在当前组装后 XNU 两个上下文文件的临时副本上确认补丁可应用，新文件与此目录逐字节一致。没有 XNU 编译或硬件结果。
