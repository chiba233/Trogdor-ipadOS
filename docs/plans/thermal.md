# SC7180 温度采集适配

已接入：`patches/xnu/coachz-prelink-thermal.patch`，`coachz-thermal=1` 打开；源码和主机夹具在 [pending/thermal](../../platform/coachz-prelink/pending/thermal/README.md)。真机 25 路全部读到；`thermalmonitord` 通过 38 个 HID 温度服务取数，不再报找不到服务。

## 选择

已有电池、背光、键盘等路径正在推进，下一项选择依赖较少的芯片温度读取。相比直接实现动态调频或 EC 传感器系统集成，TSENS 只需核对两组 MMIO 布局和有效位，且可复用公开 UniXNU 的采样思路；其输出也能成为后续温控的输入。

先保留固件状态做只读采样，而非同时初始化控制器与调整频率。若固件没启用传感器，会明确报告不可用。

## 已编写

- 两组 bank、共 25 路，原始寄存器与逐通道状态，有效温度换算为 m°C。
- 版本、控制器、reset、sensor enable、TRDY、VALID 的检查，有限重读，每轮清除旧结果。
- 有效通道峰值和约 10 秒周期日志；初始报告保留各路值与失败状态。
- 独立候选补丁和 ADT 生成函数。

移植中发现固定版本 NativeQcomTsens 的 ready 检查使用 bit 3，Linux 的 SC7180 对应 TSENS v2 定义为 bit 0。新代码按后者实现，并有专门模型检查；真机 25 路全部读到。

## 主机验证

四项检查通过：独立 C 模型（六组关键行为）、实际装载器 Node 的 ADT 序列化、候选补丁在隔离夹具中应用并与源码一致、构建接线。真机读数见 [硬件记录](../hardware-tests.md)。

## 还没做的

TSENS 初始化／校准、阈值中断、ADC_TM5 阈值监控（外壳和充电器热敏电阻改为直接轮询 ADC）、iPadOS thermal 服务的限频后端（`thermalmonitord` 没有 CLPC／PMGR）和休眠恢复。

## 按主线的温控区

上机结果：每 10 s 打出 `COACHZ thermal zones: hottest ...` 报告，空闲时最热的区约 38.8 °C，各冷却设备都在 state 0。

`coachz-prelink-thermal-zones.patch` 用主线 v6.12 `sc7180.dtsi` 的 25 个温控区和主板的 2 个热敏电阻区和 step_wise 规则（`thermal_core.c` 的 `handle_thermal_trip`、`gov_step_wise.c`、`thermal_helpers.c` 的 `__thermal_cdev_update`）替换了原来“最热通道过 80 °C 就钳制、低于 70 °C 放开”的第一版，开关仍是 `coachz-thermal-clamp=1`：

- cpu0–cpu5（tsens0 1–6）、cpu6–cpu9（tsens0 9–12）：90、95 °C 两级 passive（迟滞 2 °C），分别冷却 A55 和 A76 两个 cpufreq-hw 域；gpuss0/1（tsens0 13、14）：95 °C passive，冷却 GPU；其余区只有 90 °C hot（没有绑定冷却）。所有区 110 °C critical。
- 冷却态 s：CPU 域上限为 top − s（A55 9→0，A76 13→0，经 `coachz_cpu_state_cap` 或无调速器时直接写状态寄存器），GPU 为 7 − s（最低 1 级 180 MHz，经 `coachz_gpu_level_cap`）。一个设备取所有绑定区要求的最深一级。
- 越过 trip 后阈值降到 trip − 迟滞；超阈值且上升时加深一级，低于阈值且下降时放开。critical 连续两次读数达到时走 PSCI SYSTEM_OFF（主线是有序关机，延时后强制；这里没有有序关机）。
- 主线不在 passive 时靠 TSENS 阈值中断，这里的采样只读、不编程中断，所以平时每 1 s、有 passive 时每 250 ms 读一次，独立线程。
- 每次设备状态改变打一行 `COACHZ thermal zone <区>: <设备> state s cap c`，越过和回落各一行；每 10 s 的报告多一行 `COACHZ thermal zones: hottest ...`。
- 主板的两个热敏电阻区（`sc7180-trogdor-coachz.dtsi` 的 skin-temp、`sc7180-trogdor.dtsi` 的 charger）经 PM6150 的 ADC 读取：SPMI PMIC 仲裁器 v5（`spmi-pmic-arb.c`：APID 表、所有者 EE、observer 读、channel 写、命令格式），ADC5 单次转换（`qcom-spmi-adc5.c` 的 `adc5_do_conversion`：比率校准、抽取 840、1 次采样、200 µs 稳定时间），`qcom-vadc-common.c` 的换算（1875 mV 满量程 0x70e4，NTCG104EF104 100k 上拉表）。skin-temp 在 42、45 °C（迟滞 1 °C）冷却 A76，60 °C critical；charger 只有 125 °C critical。SPMI 写只允许发往 ADC 外设（USID 0 的 0x31）的 0x42–0x47，且 APID 必须属于本 EE；首次读数不在 −30 到 100 °C 之间就停用这两个区。主线是靠 ADC_TM5 的阈值，这里直接轮询转换。
- 原来第三次轮询时的钳制自测已去掉（钳制生效在真机上确认过）。

