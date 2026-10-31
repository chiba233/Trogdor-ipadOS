# EC motion 源码证据与批注

固定版本记录：2026-11-01。新代码按 BSD-2-Clause 发布；EC／Linux 源码用于协议、板级布局和单位核对，未搬入 Linux GPL 实现。现有 UniXNU、Sheng 和 ios-sweet 本地文档／驱动搜索没有找到可直接用于 Coachz EC motion 的系统端前端，因此这里保留自定义 IOService 契约，没有虚构 Apple motion ABI。

## 1. 实际连接与芯片选择

[Coachz board.c](https://github.com/coreboot/chrome-ec/blob/ad41bf945927e186eaad768a9d51f45ada67b9b9/board/coachz/board.c#L346-L460)，blob `c1113d4f682760830c6a1a708742df671701df8e`。

```text
LID_ACCEL: type ACCEL, location LID, EC sensor I2C, default_range 4 g
LID_GYRO:  type GYRO,  location LID, EC sensor I2C, default_range 1000 deg/s
BMI160 is the initial table; the chip-ID hook substitutes BMI260 entries.
lid_standard_ref = [[0,-1,0], [1,0,0], [0,0,1]]
```

批注：无需在 AP 侧重新初始化 BMI160／BMI260 I2C。默认量程只作板级证据，代码每轮查询量程，并按 INFO 识别 sensor ID；没有写死 accel=0、gyro=1 或套用默认量程。没有从 board.h 的 OPT3001 宏推出实装光传感器。

## 2. 命令与请求布局

[ec_commands.h](https://github.com/coreboot/chrome-ec/blob/ad41bf945927e186eaad768a9d51f45ada67b9b9/include/ec_commands.h#L2770-L3410)，blob `b237a84b19870060a2ee2113c3c854780b3b7999`。

```text
EC_CMD_MOTION_SENSE_CMD = 0x002b
subcommands: DUMP=0, INFO=1, SENSOR_ODR=3, SENSOR_RANGE=4, DATA=6
packed request = 13 bytes
byte 0: subcommand
byte 1: sensor ID (DUMP uses it as max_sensor_count; 0 requests only count)
range/ODR: byte 2 roundup, bytes 3..4 reserved, bytes 5..8 signed data
EC_MOTION_SENSE_NO_VALUE = -1
```

批注：range／ODR 不是纯 getter 命令。data 必须明确为 `ff ff ff ff`；传零会变成设置值。本客户端始终发送 -1，其他保留字节清零。已用固定源文件里的原始结构体独立编译确认 `sizeof=13`、两个 data offset 均为 5。

GET_CMD_VERSIONS `0x0008` 的 v0 请求是一个字节命令号；0x2b 能放入该字段。它返回 32 位版本掩码。客户端要求 v1 支持位，不擅自选择 v3／v4 的扩展 INFO 布局。

## 3. EC handler：DATA 并不保证回填 sensor_num

[motion_sense.c](https://github.com/coreboot/chrome-ec/blob/ad41bf945927e186eaad768a9d51f45ada67b9b9/common/motion_sense.c#L1047-L1162)，本轮直接获取的 blob `cecb1fbf14f6f505d7b40f816e822d7c4d691d94`。

该版本 DATA 分支的关键原句：

```c
out->data.flags = 0;
mutex_lock(&g_sensor_mutex);
ec_motion_sensor_fill_values(&out->data, sensor->xyz);
mutex_unlock(&g_sensor_mutex);
args->response_size = sizeof(out->data);
```

[motion_sense.h](https://github.com/coreboot/chrome-ec/blob/ad41bf945927e186eaad768a9d51f45ada67b9b9/include/motion_sense.h#L386-L395)，blob `1df9f1f631778d6af9a5f9069a98800a46ea13dd`：该 helper 只把三个轴赋给 `dst->data[0..2]`，没有填写 `sensor_num`。

批注：头文件里虽有 sensor_num 成员，不能要求它等于请求的编号，否则 gyro 等非零编号可能被拒绝。响应归属由公共 EC transport 的串行命令边界确定。DATA 的 flags=0 也不能拿 DUMP 的 PRESENT bit 判无效。当前客户端要求 DATA 恰为 8 字节且 flags=0，遇到其他 frame 标记明确拒绝，不把时间戳帧解成三轴。

同一 EC 文件的 `host_sensor_id_to_real_sensor()` 还要求 SENSOR_ACTIVE 且 SENSOR_READY；INFO／DATA 可能在初始化期间失败，因此服务有有界重试，未伪造“传感器存在即有读数”。handler 声明版本 1、2、3、4；v1 INFO 响应为三个字节。

## 4. ODR 与量程查询的真实语义

[motion_sense.c 的 ODR／range 分支](https://github.com/coreboot/chrome-ec/blob/ad41bf945927e186eaad768a9d51f45ada67b9b9/common/motion_sense.c#L1192-L1240)：只有参数不等于 NO_VALUE 时才设置；查询分别返回驱动的实际 data rate 和 current_range。

```text
ODR ret: sensor->drv->get_data_rate(sensor), mHz
range ret: sensor->current_range, ±g or ±deg/s
```

批注：ODR=0 是合法的停用状态，不能把缓存里可能残留的三个零或旧向量当作工作中的 gyro。此驱动不为了取得值而改采样率。

## 5. 单位和坐标

[Linux cros_ec_sensors.c](https://github.com/torvalds/linux/blob/36aad21d50d1af3f6954bb0c988257a908202477/drivers/iio/common/cros_ec_sensors/cros_ec_sensors.c#L96-L125)，本轮获取的 blob `551b86236abccae113fff5d40d74a9abf537266a`。其 16 位有符号数据满量程分母为 `2^(16-1)`；加速度范围以 g、角速度范围以 deg/s 提供，Linux 再分别换成 m/s² 和 rad/s。

本客户端使用便于检查的整数单位：

```c
scaled = (int64_t)raw * queried_range * 1000000 / 32768;
/* accel: micro-g; gyro: micro-degrees/second, truncation toward zero */
```

[EC accelgyro_bmi_common.c](https://github.com/coreboot/chrome-ec/blob/ad41bf945927e186eaad768a9d51f45ada67b9b9/driver/accelgyro_bmi_common.c#L330-L350)，blob `f651a4dd1085117526fb520f0a1158e127818c49`：原始 int16 XYZ 经过 `rotate(v, *s->rot_standard_ref, v)` 和 scale 处理。客户端保留这些结果，不重复应用板级矩阵。屏幕坐标与 iPadOS rotation 定义的映射仍待后续接口工作。

## 6. 现有公共 EC 所有权

本仓库 `coachz-prelink-battery.patch` 提供 `coachz_ec_command()`：请求上限 32、响应上限 64 字节，互斥锁覆盖完整一次 SPI 交易，并验证协议长度、checksum 和 EC result。新增代码只链接这个接口，13 字节请求和最大 8 字节实际响应可容纳；不另写控制器初始化、绕过锁或消费 MKBP/FIFO 事件。并发负载与真实传感器状态仍待实机核对。
