# EC 光感与自动亮度（pending，未接入）

> 这台 Coachz 没有光感（Debian 的 IIO 只有加速度计和陀螺仪），这份驱动留着不接入，给有 EC 光感的设备用。

这里仅有待接入源码和说明。没有修改活动内核、编译参数、补丁列表、打包脚本或其他驱动；没有编译、执行测试、连接设备、写盘或提交。

## 先说明板级限制

**当前无法确认 Coachz 存在可用的环境光传感器。** 已核对主线 Linux 的 EC 光感驱动、Coachz/Trogdor DTS，以及上游 EC 板级配置：

- Linux 的 `cros_ec_light_prox` 通过 EC 的 `MOTIONSENSE_TYPE_LIGHT=4` 读取**已换算的无符号 lux**。
- 当前 Coachz EC 配置只注册 `LID_ACCEL` 和 `LID_GYRO`，替换用的 BMI260 表也只有这两项，没有 LIGHT。Trogdor/Coachz 的相关 DTS 没有给出另一条独立 ALS 节点。
- `board/coachz/board.h` 虽然有 `OPT3001_I2C_ADDR_FLAGS`，但这不是 ALS 驱动启用或光感存在的证据。检查的 baseboard 中也没有对应 `CONFIG_ALS` 启用。
- Coachz 的 EC proximity、底座/支架/body-detection，以及 AP 侧 SAR proximity 节点不能当作光照强度。

上述是源码中的暴露情况，**不能据此断言所有工程板都物理上没有光感**。实现枚举真实 LIGHT；没有时报告 unavailable，不尝试一个猜测的 sensor ID，也不读可能全零的 ALS memmap 来制造“0 lux”。固定源码和批注见 [SOURCES.md](SOURCES.md)。

## 已写内容

| 文件 | 内容 |
| --- | --- |
| `coachz_als.h` | EC v1 协议客户端：查询能力、枚举、选择唯一 lid LIGHT、查询 ODR、读取 processed lux |
| `coachz_auto_brightness.h` | 纯整数的可选本地策略：滤波、阈值、防抖、延迟调暗、手动暂停、输入中断后重新建立基线 |
| `CoachzAmbientLight.cpp` | XNU 工作循环、EC 查询和状态发布；通过现有 AppleARMBacklight 服务请求亮度，不直接写 PWM |
| `ambient_light_model_test.c` | 协议与策略模型测试源码；未运行、未加入任何测试列表 |
| `INTEGRATION.md` | 以后接入时的依赖、所有权和验收说明 |

自动调光默认关闭，并要求集成者显式授予唯一控制权。当前 nominal 曲线是示例偏好，不是 Linux 板级参数、面板实测校准或 Apple 的自动亮度算法。无 LIGHT、采样停用、低于 1 Hz、EC 错误、背光属性不可读、熄屏或未获控制权时不会发调光请求。

**这不是已接通的原生 CoreBrightness ALS 服务。** 它不假冒 Apple 传感器或 HID，不会让设置中的原生自动亮度开关凭空可用。当前提供的是诊断 lux 服务和可明确启用的本地调光后端；原生 ALS 的匹配、事件和订阅 ABI 需要另行资格核对。原有手动亮度服务保持其现状。

## 实现边界

- 不直接访问 EC 私有 I2C，不设置传感器量程、校准或 ODR，不读取/消费共享 FIFO。
- 若真实 LIGHT 的 ODR 为零，由未来的公共传感器 owner 协调激活；当前代码不会为了开启自动亮度改动 IMU。
- EC DATA 是最新缓存值，没有可用于此请求的硬件采样时间。HostCompletionMs 是读取完成时间，不代表新的转换。0 lux 是有效黑暗值，错误不能解释成0。
- 本地策略检测到其他亮度请求或收到手动通知后暂停30秒；原生 CoreBrightness 自动控制与本地控制不能同时作为写入者。
- 背光 setProperties 成功仅代表请求被接受；实际 PWM 写入与回读仍由已有背光 worker 完成。
- 尚未验证 XNU 编译、硬件光感、环境响应、实际亮度、休眠恢复或系统自动亮度 UI。
