# Chrome EC 加速度计／陀螺仪（待接入）

本目录提供协议客户端和 IOKit 采样服务，**没有加入活动内核构建**。没有编译 XNU 对象、打包、连接设备或写盘，硬件结果尚未取得。

## 已实现

- 复用 `coachz_ec_command()`，不另开 SPI／I2C 驱动，不绕过公共总线锁。
- 查询 `GET_CMD_VERSIONS`，明确选择 motion 命令 v1；通过 count-only DUMP 和逐个 INFO 枚举，不猜传感器编号。
- 支持 INFO 返回的 lid 位置 BMI160／BMI260 加速度计和陀螺仪。最多扫描 8 个 EC 传感器；其他类型仅发布元数据和 unsupported 状态。
- 约每 200 ms 查询实际 ODR、量程和最新三轴值。ODR 为 0 时报告 inactive，不发送启用命令；负值、无效量程、短响应和传输错误均不生成轴数据。
- 发布控制器 `CoachzECMotion` 及传感器子服务，以不可变 `LatestSample` 字典整体替换快照。初始化错误最多重试 5 次，间隔 2 秒。
- 保留 EC 已应用的标准参考坐标，不再重复旋转矩阵。

没有 FIFO_READ／GET_NEXT_EVENT，不取走按键等消费者的共享事件。没有写采样率、量程、校准、spoof、EC 固件或传感器配置。传感器没开时会明确保持不可用。

## 代码与接入

- `coachz_motion.h`：无内核依赖的协议和采样逻辑，也是主机模型检查的对象。
- `CoachzMotion.cpp`：IOService、工作循环、定时采样、子服务和快照发布。
- `coachz-prelink-motion.patch`：候选补丁，新增两个文件至 `iokit/Kernel`，登记 `iokit/conf/files`，在既有 sampler 调用 `coachz_motion_start()`；未加入活动 `series`。
- [SOURCES.md](SOURCES.md)：固定版本源码、请求布局、DATA 字段差异及换算批注。

接手者自行选择接入并构建时，在已有电池／公共 EC 补丁之后应用候选补丁，再决定是否加入 `coachz-motion=1`。不需要改 ADT，也不依赖待接入的 Wi-Fi／TSENS；没有启动参数时不访问 EC。此处不调用打包或写盘流程。

## 可读取的服务契约

子服务有 `ECSensorID`、`ECSensorType`、`ECSensorLocation`、`ECSensorChip`、`Coordinates` 和 `SupportedByThisDriver` 元数据。它们是本驱动自定义的诊断契约，**不是 Apple 的 CoreMotion ABI**。

`LatestSample` 字典：

| 字段 | 语义 |
| --- | --- |
| Status / Error | 本轮查询状态／错误；EC 传输错误原样保留，本地解析错误为负值 |
| HostSequence | 主机轮询序号，不是 EC 的采样序号 |
| HostCompletionNS | 主机完成这些命令时的 uptime 纳秒，不是传感器时间戳 |
| ODRMilliHz | 查询到的实际采样频率，单位 mHz；0 表示停用 |
| Range | 加速度计 ±g；陀螺仪 ±deg/s，来自本轮查询 |
| RawXYZ | 仅查询成功时存在，三个有符号原始值 |
| ScaledXYZ / Units | 加速度计 micro-g；陀螺仪 micro-degrees-per-second，**不是 rad/s** |

带符号整数作为 64 位 OSNumber 的二进制补码存储，读取者须按 `int64_t` 解读。每轮失败快照省略 RawXYZ／ScaledXYZ，不能把缺失的值解释为零。若内存分配或属性替换失败，旧快照保留原来的序号／时间，不冒充本轮结果。

DATA 是 EC 缓存的最新值，可能重复，且没有硬件时间戳或保证新鲜度的序号；本服务的 available 表示命令和数据格式通过检查。量程／ODR 查询与 DATA 是分开的命令，不构成跨命令原子配置快照。未来增加配置写入者时，需要由公共 motion owner 协调，不能认为当前换算解决了重配置竞争。

## 验证边界和实机步骤

主机四项检查：协议／采样 C 模型（六组行为）、独立头文件的 C++ 语法、隔离候选补丁应用并比对源码、活动构建未启用。还从固定版 EC 原始结构体独立验证了请求大小 13、range／ODR data 字段偏移 5；主机编译的只有这些独立程序。

```sh
python3 -B -m unittest discover -s tests -p test_motion.py -v
```

接入活动构建时，应同步更新该测试中“尚未启用”的阶段断言；协议和采样模型检查继续保留。

后续由接手者编译并上机后：

1. 记录命令版本、传感器数量及逐个 INFO 的 type／location／chip，确认工程板实际枚举。
2. 检查 ODR 和量程；未启用的 gyro 先记录，不算采样已通过。
3. 比较静止、翻面和倾斜时的加速度原始值及换算值；对照 Debian 同 sensor ID 的数据，确认坐标和约 1g 的重力幅值。
4. 只有 gyro 已启用且有真实读数时，才验证其转动响应与 deg/s 单位。
5. 观察电池、背光、按键持续工作及 EC 错误情况。轮询频率仅是初始选择，实际开销仍待实机测量。

已提供定时器停止和子服务退出路径，但没有运行 IOKit 生命周期测试或验证睡眠恢复。自动旋转、CoreMotion 应用接口、FIFO 中断和硬件时间同步仍需后续适配；此轮不涉及蓝牙或手写笔。
