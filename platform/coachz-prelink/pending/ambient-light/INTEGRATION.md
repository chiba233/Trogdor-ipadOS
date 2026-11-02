# 接入契约（尚未应用）

## 确认输入来源

先读取真实设备的 EC sensor INFO 和 Linux IIO 枚举。当前公开 Coachz EC 表只有加速度计/陀螺仪，因此不能预先宣称本机有 ALS。若 EC 没有 type=4 的 LIGHT，停在明确的 unavailable：这份代码不能凭空增加物理传感器。若工程板用独立 AP I2C 光感，应取得它在 Linux 下的驱动、设备树和准确型号，再实现那条后端，不扫描猜测地址。

`ca_discover()` 支持明确的 MOTIONSENSE v1，小 INFO 为3字节，DUMP(count-only)为2字节。枚举上限32，超出直接返回限制错误，不在不完整枚举中偷偷选择设备。只有唯一的 lid LIGHT 被自动选择；多个 LIGHT 或其他位置需板级 owner 显式选择实际的前向光感。不能固定假设它是 sensor2。

采集查询 ODR（data=-1，request byte5..8），不写采样率。非零 ODR 才读取 DATA；本地策略还要求至少1000 mHz。实际的开启、关闭、恢复、共享订阅计数归公共传感器 owner 管理；不能越过它重设 motion FIFO 或所有传感器的频率。

DATA 的前两字节是 flags/sensor_num，data[0] 在 byte2..3。Linux 将这一项按 unsigned16 解释为 processed lux。无需乘 SENSOR_RANGE，更不能把它套进加速度的 signed16/32768 换算。flags 和 DATA sensor_num 不用作虚构的 PRESENT 位或时间戳。

## XNU 服务

未来可将 `CoachzAmbientLight.cpp` 与两个头文件纳入接入者选择的音频/背光之外的独立驱动配方。**本次没有创建或修改配方**。还需为选定 XNU 做正常的类注册、静态构造与初始化接入；这里没有自动启动钩子。

文件导出的 `coachz_ambient_light_allocate()` 只分配服务。完成其 metaclass 注册后，由 owner 调用 `init/attach/start`，provider 必须是已经工作的 `AppleARMBacklight` 子类，即现有 CoachzBacklight。服务持有 provider，复用公共 `coachz_ec_command()`。不另外创建 SPI owner，不操作 GPIO12 或背光门控。

成功 start 后注册 `CoachzECAmbientLight`，开始查询但不自动调光。选中传感器时500ms轮询；没有或枚举失败时5秒重试；熄屏时暂停 EC 查询。该 IOService 是自定义诊断接口，**不是 Apple ALS 事件源**。

`AmbientLightState` 为整体替换的不可变字典，读者可订阅 property-change 通知再取新快照：

| 字段 | 语义 |
| --- | --- |
| Status / Error | 本轮读取状态，传输错误原样保留 |
| EnumerationError | 枚举/选择失败；无设备与多设备分别报告 |
| ReportedSensors / LightSensors | EC总数、完整枚举中的LIGHT数量 |
| ECSensorID / ECSensorChip | 仅实际选择后存在，不硬编码 |
| IlluminanceLux | 仅成功采样时存在，0有效 |
| ODRMilliHz | 查询到的采样频率；暂停/未读时不作为实时频率使用 |
| HostSequence / HostCompletionMs | 发布序号、主机读完的uptime；不是传感器采样时间 |
| LocalAutoBrightnessEnabled / LocalControlGranted / DisplayAwake | 本地策略开关、独占写入许可和显示状态 |
| FilteredMilliLux / ManualHoldUntilMs | 本地策略诊断 |
| BacklightRequestError / AcceptedBacklightRequests | 服务请求结果/接受次数，**不是PWM成功次数** |

不能从缺失的 IlluminanceLux 推断黑暗；错误发布会省略它。若字典分配/替换失败，原属性保持原序号，不能把它当新采样。

## 本地自动亮度与所有权

默认不执行。以下 `setProperties` 键用于未来的明确控制层，值均为OSBoolean：

- `LocalControlGranted`：集成者确认本地策略是唯一自动写入者。撤回会同时禁用策略。
- `LocalAutoBrightnessEnabled`：启用本地调光；没有上述许可时拒绝启用。可以与授予许可在同一个字典提交。
- `DisplayAwake`：睡眠/合盖/熄屏控制层必须同步它。false 时不访问光感，也不调亮屏幕；恢复后重新建立滤波基线。
- `ManualBrightnessChanged`：true 通知一次手动调节，暂停自动控制30秒。亮度键、滑块等每次手动操作都应通知，以延长暂停；只靠对比当前亮度不能恢复所有操作历史。

所有配置与采样在同一个workloop gate下串行。关闭/撤回不改当前亮度。尚在现有背光服务队列里的旧渐变不在本服务控制范围内；若切换写入者时必须立即打断，切换层应向背光发送新的实际目标以替换它。

策略使用现有 `IODisplayParameters/BrightnessMilliNits/value` 读取请求目标，经 `brightness-nits` 与 `FadePeriod` 的16.16格式向 provider 提交。它不调用EC PWM接口，不更改原背光的20..400 nominal nits范围。数字来自已有后端的声明，未经面板仪器校准。

默认示例曲线在 `coachz_auto_brightness.h`：0lux→20nits、100lux→110nits、1000lux→260nits、5000lux以上→400nits。仅是本地策略偏好，交付前应校准/调整，不是从Linux或Apple提取的校准表。上升滤波常数1秒、下降3秒；亮度差阈值为5nits与当前5%的较大者，持续上升1秒/下降5秒才请求一次1秒渐变。间隔超过2秒或出现错误就重建输入基线。

不要同时把同一lux流交给原生CoreBrightness自动控制又启用这里的本地闭环。正常的长期路线是核对实际iPadOS的ALS服务/HID事件ABI，让原生策略消费真实lux；本实现没有完成该ABI，也没有修改系统自动亮度偏好。

## 生命周期与以后验收

stop在gate中取消并禁用timer，再移除event sources并释放provider；未接入系统电源管理，睡眠/恢复通知仍需由调用方接通。stop及重启、和其他EC消费者的并发、实际CoreBrightness竞争均未运行验证。

本轮只写文件，下列检查均留给以后执行：

1. 主机编译/运行 `ambient_light_model_test.c`，核对无LIGHT、误把PROX当LIGHT、无符号lux、0lux、ODR0、短包/错误、多LIGHT、滤波、防抖、手动暂停。
2. 在项目实际XNU头文件和链接符号下编译服务，确认IOKit生命周期；不得把桩类型检查当内核编译通过。
3. 真机先确认LIGHT确实存在且可运行，和Debian的 `in_illuminance_input` 同单位对照；遮挡和照明时有合理响应，错误不变成0lux。
4. 只开诊断状态验证EC链路不影响按键、背光、电池和IMU，再明确授予本地调光许可。
5. 检查手动滑块/亮度键暂停、调暗延迟、最暗仍亮、熄屏不误唤醒；核对实际PWM回读和可见亮度，而非只看请求次数。
6. 原生自动亮度UI、CoreBrightness事件订阅与策略验收单独记录；当前不能宣称已支持。
