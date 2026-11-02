# 光感与自动亮度：源码证据与批注

这里只记录源码和未接入实现，不代表本机已有可用光感或自动亮度。主线 Linux 负责硬件协议，UniXNU 用于系统亮度接口参考；本地调光曲线是实现策略，不伪装成上游硬件校准。

## 固定来源

- Linux：`36aad21d50d1af3f6954bb0c988257a908202477`，torvalds/linux。
- Chrome EC：`ad41bf945927e186eaad768a9d51f45ada67b9b9`，coreboot/chrome-ec 镜像中的 ChromiumOS EC 源码。它是 EC 板级配置，不是拿 coreboot AP 启动代码替代 Linux 驱动；先读 Linux 后才以它核对是否注册了传感器。
- 以下保留原始行号、blob SHA和摘录。Linux 摘录属于原文件的 GPL 许可证和作者；EC 摘录保留其 ChromiumOS BSD 归属。新驱动是独立表达，摘录不参加编译。完整许可/版权头可从固定链接查看。

## 结论与实现映射

| 已核对事实 | 本次实现 |
| --- | --- |
| Linux LIGHT=4；PROX=3 | 只把真实 LIGHT 当 lux，绝不把 proximity/body-detection 当环境光 |
| LIGHT 单通道为 unsigned16、PROCESSED lux | DATA byte2..3 直接读取，不 signed 扩展，不重复乘 range/scale |
| 通过 sensorhub 的 INFO 逐个枚举 | 无固定sensor ID；选择唯一lid LIGHT；无光感/多光感分别报告 |
| 通过 cros_ec_sensors_read_cmd 读取缓存 DATA | 复用公共EC接口；不另开I2C、不消费FIFO、无硬件时间戳声明 |
| ODR 的查询值 NO_VALUE=-1 | request byte5..8置ff，不意外写采样率 |
| Coachz EC motion 表只有 accel/gyro | 没有把LIGHT作为板级已确认设备；无LIGHT即unavailable |
| OPT3001 地址宏存在，但没有对应注册 | 不把一个宏当实际硬件和驱动存在的证据 |
| 当前背光接受16.16 nits和FadePeriod | 本地策略仅向现有背光服务提交，不直接发EC PWM写入 |

1 Hz的本地控制门槛、500ms轮询、滤波、防抖、手动暂停以及20..400 nominal nits曲线均为本实现的策略/接口选择，不是Linux ALS硬件要求。

## 1. 主线光感读取和单位

[drivers/iio/light/cros_ec_light_prox.c](https://github.com/torvalds/linux/blob/36aad21d50d1af3f6954bb0c988257a908202477/drivers/iio/light/cros_ec_light_prox.c)  
blob `b1c92de7922af53c8116adbcdd3ef322832a8766`；源文件许可标识：// SPDX-License-Identifier: GPL-2.0 /*  * cros_ec_light_prox - Driver for light and prox sensors behing CrosEC.  *  * Copyright (C) 2017 Google, Inc

批注：这是processed lux，不能照搬加速度计scaled/raw换算；unsigned16允许大于32767的亮度。RGB/色温未从单路lux伪造。

[L25–L29](https://github.com/torvalds/linux/blob/36aad21d50d1af3f6954bb0c988257a908202477/drivers/iio/light/cros_ec_light_prox.c#L25-L29)

```text
25:  * light sensors to return the what the eye would see. For proximity, we
26:  * currently support only one light source.
27:  */
28: #define CROS_EC_LIGHT_PROX_MAX_CHANNELS (1 + 1)
29: 
```

[L63–L80](https://github.com/torvalds/linux/blob/36aad21d50d1af3f6954bb0c988257a908202477/drivers/iio/light/cros_ec_light_prox.c#L63-L80)

```text
63: 	case IIO_CHAN_INFO_PROCESSED:
64: 		if (chan->type == IIO_LIGHT) {
65: 			ret = cros_ec_sensors_read_cmd(indio_dev, 1 << idx,
66: 						     (s16 *)&data);
67: 			if (ret)
68: 				break;
69: 			/*
70: 			 * The data coming from the light sensor is
71: 			 * pre-processed and represents the ambient light
72: 			 * illuminance reading expressed in lux.
73: 			 */
74: 			*val = data;
75: 			ret = IIO_VAL_INT;
76: 		} else {
77: 			ret = -EINVAL;
78: 		}
79: 		break;
80: 	case IIO_CHAN_INFO_CALIBBIAS:
```

[L195–L225](https://github.com/torvalds/linux/blob/36aad21d50d1af3f6954bb0c988257a908202477/drivers/iio/light/cros_ec_light_prox.c#L195-L225)

```text
195: 	channel->info_mask_shared_by_all_available =
196: 		BIT(IIO_CHAN_INFO_SAMP_FREQ);
197: 	channel->scan_type.realbits = CROS_EC_SENSOR_BITS;
198: 	channel->scan_type.storagebits = CROS_EC_SENSOR_BITS;
199: 	channel->scan_type.shift = 0;
200: 	channel->scan_index = 0;
201: 	channel->ext_info = cros_ec_sensors_ext_info;
202: 	channel->scan_type.sign = 'u';
203: 
204: 	/* Sensor specific */
205: 	switch (state->core.type) {
206: 	case MOTIONSENSE_TYPE_LIGHT:
207: 		channel->type = IIO_LIGHT;
208: 		channel->info_mask_separate =
209: 			BIT(IIO_CHAN_INFO_PROCESSED) |
210: 			BIT(IIO_CHAN_INFO_CALIBBIAS) |
211: 			BIT(IIO_CHAN_INFO_CALIBSCALE);
212: 		break;
213: 	case MOTIONSENSE_TYPE_PROX:
214: 		channel->type = IIO_PROXIMITY;
215: 		channel->info_mask_separate =
216: 			BIT(IIO_CHAN_INFO_RAW) |
217: 			BIT(IIO_CHAN_INFO_CALIBBIAS) |
218: 			BIT(IIO_CHAN_INFO_CALIBSCALE);
219: 		break;
220: 	default:
221: 		dev_warn(dev, "Unknown motion sensor\n");
222: 		return -EINVAL;
223: 	}
224: 
225: 	channel++;
```

## 2. 读取 helper 必须读到实现

[drivers/iio/common/cros_ec_sensors/cros_ec_sensors_core.c](https://github.com/torvalds/linux/blob/36aad21d50d1af3f6954bb0c988257a908202477/drivers/iio/common/cros_ec_sensors/cros_ec_sensors_core.c)  
blob `d03e40c9158266f6f4c92ddb331539839c3459c1`；源文件许可标识：// SPDX-License-Identifier: GPL-2.0 /*  * cros_ec_sensors_core - Common function for Chrome OS EC sensor driver.  *  * Copyright (C) 2016 Google, Inc

批注：主线helper用MOTIONSENSE_CMD_DATA和data[i]，ODR读取用NO_VALUE。返回值是EC缓存最新值，代码没有从DATA获得一个新的硬件采样时间。

[L408–L444](https://github.com/torvalds/linux/blob/36aad21d50d1af3f6954bb0c988257a908202477/drivers/iio/common/cros_ec_sensors/cros_ec_sensors_core.c#L408-L444)

```text
408: int cros_ec_motion_send_host_cmd(struct cros_ec_sensors_core_state *state,
409: 				 u16 opt_length)
410: {
411: 	struct ec_response_motion_sense *resp = (struct ec_response_motion_sense *)state->msg->data;
412: 	int ret;
413: 
414: 	if (opt_length)
415: 		state->msg->insize = min(opt_length, state->ec->max_response);
416: 	else
417: 		state->msg->insize = state->ec->max_response;
418: 
419: 	memcpy(state->msg->data, &state->param, sizeof(state->param));
420: 
421: 	ret = cros_ec_cmd_xfer_status(state->ec, state->msg);
422: 	trace_cros_ec_motion_host_cmd(&state->param, resp, ret);
423: 	if (ret < 0)
424: 		return ret;
425: 
426: 	if (ret && state->resp != resp)
427: 		memcpy(state->resp, resp, ret);
428: 
429: 	return 0;
430: }
431: EXPORT_SYMBOL_GPL(cros_ec_motion_send_host_cmd);
432: 
433: static ssize_t cros_ec_sensors_calibrate(struct iio_dev *indio_dev,
434: 		uintptr_t private, const struct iio_chan_spec *chan,
435: 		const char *buf, size_t len)
436: {
437: 	struct cros_ec_sensors_core_state *st = iio_priv(indio_dev);
438: 	int ret, i;
439: 	bool calibrate;
440: 
441: 	ret = kstrtobool(buf, &calibrate);
442: 	if (ret < 0)
443: 		return ret;
444: 
```

[L675–L698](https://github.com/torvalds/linux/blob/36aad21d50d1af3f6954bb0c988257a908202477/drivers/iio/common/cros_ec_sensors/cros_ec_sensors_core.c#L675-L698)

```text
675: int cros_ec_sensors_read_cmd(struct iio_dev *indio_dev,
676: 			     unsigned long scan_mask, s16 *data)
677: {
678: 	struct cros_ec_sensors_core_state *st = iio_priv(indio_dev);
679: 	int ret;
680: 	unsigned int i;
681: 
682: 	/* Read all sensor data through a command. */
683: 	st->param.cmd = MOTIONSENSE_CMD_DATA;
684: 	ret = cros_ec_motion_send_host_cmd(st, sizeof(st->resp->data));
685: 	if (ret != 0) {
686: 		dev_warn(&indio_dev->dev, "Unable to read sensor data\n");
687: 		return ret;
688: 	}
689: 
690: 	for_each_set_bit(i, &scan_mask, iio_get_masklength(indio_dev)) {
691: 		*data = st->resp->data.data[i];
692: 		data++;
693: 	}
694: 
695: 	return 0;
696: }
697: EXPORT_SYMBOL_GPL(cros_ec_sensors_read_cmd);
698: 
```

[L760–L779](https://github.com/torvalds/linux/blob/36aad21d50d1af3f6954bb0c988257a908202477/drivers/iio/common/cros_ec_sensors/cros_ec_sensors_core.c#L760-L779)

```text
760: 	int ret, frequency;
761: 
762: 	switch (mask) {
763: 	case IIO_CHAN_INFO_SAMP_FREQ:
764: 		st->param.cmd = MOTIONSENSE_CMD_SENSOR_ODR;
765: 		st->param.sensor_odr.data =
766: 			EC_MOTION_SENSE_NO_VALUE;
767: 
768: 		ret = cros_ec_motion_send_host_cmd(st, 0);
769: 		if (ret)
770: 			break;
771: 
772: 		frequency = st->resp->sensor_odr.ret;
773: 		*val = frequency / 1000;
774: 		*val2 = (frequency % 1000) * 1000;
775: 		ret = IIO_VAL_INT_PLUS_MICRO;
776: 		break;
777: 	default:
778: 		ret = -EINVAL;
779: 		break;
```

## 3. sensorhub 区分 LIGHT 与 PROX

[drivers/platform/chrome/cros_ec_sensorhub.c](https://github.com/torvalds/linux/blob/36aad21d50d1af3f6954bb0c988257a908202477/drivers/platform/chrome/cros_ec_sensorhub.c)  
blob `a8395ab0a12a0aa0b288739b7ccf6d81f1dd190e`；源文件许可标识：// SPDX-License-Identifier: GPL-2.0 /*  * Sensor HUB driver that discovers sensors behind a ChromeOS Embedded  * Controller.  *

批注：传感器类型由EC返回的INFO决定，不由设备名、地址宏或底座状态推断。

[L65–L83](https://github.com/torvalds/linux/blob/36aad21d50d1af3f6954bb0c988257a908202477/drivers/platform/chrome/cros_ec_sensorhub.c#L65-L83)

```text
65: 	for (i = 0; i < sensorhub->sensor_num; i++) {
66: 		sensorhub->params->cmd = MOTIONSENSE_CMD_INFO;
67: 		sensorhub->params->info.sensor_num = i;
68: 
69: 		retries = CROS_EC_CMD_INFO_RETRIES;
70: 		do {
71: 			ret = cros_ec_cmd_xfer_status(ec->ec_dev, msg);
72: 			if (ret == -EBUSY) {
73: 				/* The EC is still busy initializing sensors. */
74: 				usleep_range(5000, 6000);
75: 				retries--;
76: 			}
77: 		} while (ret == -EBUSY && retries);
78: 
79: 		if (ret < 0) {
80: 			dev_err(dev, "no info for EC sensor %d : %d/%d\n",
81: 				i, ret, msg->result);
82: 			continue;
83: 		}
```

[L89–L118](https://github.com/torvalds/linux/blob/36aad21d50d1af3f6954bb0c988257a908202477/drivers/platform/chrome/cros_ec_sensorhub.c#L89-L118)

```text
89: 		switch (sensorhub->resp->info.type) {
90: 		case MOTIONSENSE_TYPE_ACCEL:
91: 			name = "cros-ec-accel";
92: 			break;
93: 		case MOTIONSENSE_TYPE_BARO:
94: 			name = "cros-ec-baro";
95: 			break;
96: 		case MOTIONSENSE_TYPE_GYRO:
97: 			name = "cros-ec-gyro";
98: 			break;
99: 		case MOTIONSENSE_TYPE_MAG:
100: 			name = "cros-ec-mag";
101: 			break;
102: 		case MOTIONSENSE_TYPE_PROX:
103: 			name = "cros-ec-prox";
104: 			break;
105: 		case MOTIONSENSE_TYPE_LIGHT:
106: 			name = "cros-ec-light";
107: 			break;
108: 		case MOTIONSENSE_TYPE_ACTIVITY:
109: 			name = "cros-ec-activity";
110: 			break;
111: 		default:
112: 			dev_warn(dev, "unknown type %d\n",
113: 				 sensorhub->resp->info.type);
114: 			continue;
115: 		}
116: 
117: 		ret = cros_ec_sensorhub_allocate_sensor(dev, name, i);
118: 		if (ret)
```

## 4. 协议结构与字段

[include/linux/platform_data/cros_ec_commands.h](https://github.com/torvalds/linux/blob/36aad21d50d1af3f6954bb0c988257a908202477/include/linux/platform_data/cros_ec_commands.h)  
blob `a7bd69d8352227cdabb317e8e85d6867907aaab1`；源文件许可标识：/* SPDX-License-Identifier: GPL-2.0-only */ /*  * Host communication command constants for ChromeOS EC  *  * Copyright (C) 2012 Google, Inc

批注：union在command后，sensor_odr.data位于整体request byte5。INFO v1使用小响应；DATA响应头后首个16位data为lux。13字节请求沿用当前EC协议结构和已有motion客户端，不在此导入Linux结构体。

[L1064–L1087](https://github.com/torvalds/linux/blob/36aad21d50d1af3f6954bb0c988257a908202477/include/linux/platform_data/cros_ec_commands.h#L1064-L1087)

```text
1064: /* Read versions supported for a command */
1065: #define EC_CMD_GET_CMD_VERSIONS 0x0008
1066: 
1067: /**
1068:  * struct ec_params_get_cmd_versions - Parameters for the get command versions.
1069:  * @cmd: Command to check.
1070:  */
1071: struct ec_params_get_cmd_versions {
1072: 	uint8_t cmd;
1073: } __ec_align1;
1074: 
1075: /**
1076:  * struct ec_params_get_cmd_versions_v1 - Parameters for the get command
1077:  *         versions (v1)
1078:  * @cmd: Command to check.
1079:  */
1080: struct ec_params_get_cmd_versions_v1 {
1081: 	uint16_t cmd;
1082: } __ec_align2;
1083: 
1084: /**
1085:  * struct ec_response_get_cmd_versions - Response to the get command versions.
1086:  * @version_mask: Mask of supported versions; use EC_VER_MASK() to compare with
1087:  *                a desired version.
```

[L2321–L2339](https://github.com/torvalds/linux/blob/36aad21d50d1af3f6954bb0c988257a908202477/include/linux/platform_data/cros_ec_commands.h#L2321-L2339)

```text
2321: #define EC_CMD_MOTION_SENSE_CMD 0x002B
2322: 
2323: /* Motion sense commands */
2324: enum motionsense_command {
2325: 	/*
2326: 	 * Dump command returns all motion sensor data including motion sense
2327: 	 * module flags and individual sensor flags.
2328: 	 */
2329: 	MOTIONSENSE_CMD_DUMP = 0,
2330: 
2331: 	/*
2332: 	 * Info command returns data describing the details of a given sensor,
2333: 	 * including enum motionsensor_type, enum motionsensor_location, and
2334: 	 * enum motionsensor_chip.
2335: 	 */
2336: 	MOTIONSENSE_CMD_INFO = 1,
2337: 
2338: 	/*
2339: 	 * EC Rate command is a setter/getter command for the EC sampling rate
```

[L2456–L2472](https://github.com/torvalds/linux/blob/36aad21d50d1af3f6954bb0c988257a908202477/include/linux/platform_data/cros_ec_commands.h#L2456-L2472)

```text
2456: enum motionsensor_type {
2457: 	MOTIONSENSE_TYPE_ACCEL = 0,
2458: 	MOTIONSENSE_TYPE_GYRO = 1,
2459: 	MOTIONSENSE_TYPE_MAG = 2,
2460: 	MOTIONSENSE_TYPE_PROX = 3,
2461: 	MOTIONSENSE_TYPE_LIGHT = 4,
2462: 	MOTIONSENSE_TYPE_ACTIVITY = 5,
2463: 	MOTIONSENSE_TYPE_BARO = 6,
2464: 	MOTIONSENSE_TYPE_SYNC = 7,
2465: 	MOTIONSENSE_TYPE_MAX,
2466: };
2467: 
2468: /* List of motion sensor locations. */
2469: enum motionsensor_location {
2470: 	MOTIONSENSE_LOC_BASE = 0,
2471: 	MOTIONSENSE_LOC_LID = 1,
2472: 	MOTIONSENSE_LOC_CAMERA = 2,
```

[L2620–L2633](https://github.com/torvalds/linux/blob/36aad21d50d1af3f6954bb0c988257a908202477/include/linux/platform_data/cros_ec_commands.h#L2620-L2633)

```text
2620: struct ec_params_motion_sense {
2621: 	uint8_t cmd;
2622: 	union {
2623: 		/* Used for MOTIONSENSE_CMD_DUMP. */
2624: 		struct __ec_todo_unpacked {
2625: 			/*
2626: 			 * Maximal number of sensor the host is expecting.
2627: 			 * 0 means the host is only interested in the number
2628: 			 * of sensors controlled by the EC.
2629: 			 */
2630: 			uint8_t max_sensor_count;
2631: 		} dump;
2632: 
2633: 		/*
```

[L2643–L2673](https://github.com/torvalds/linux/blob/36aad21d50d1af3f6954bb0c988257a908202477/include/linux/platform_data/cros_ec_commands.h#L2643-L2673)

```text
2643: 		/*
2644: 		 * Used for MOTIONSENSE_CMD_INFO, MOTIONSENSE_CMD_DATA
2645: 		 */
2646: 		struct __ec_todo_unpacked {
2647: 			uint8_t sensor_num;
2648: 		} info, info_3, data, fifo_flush, list_activities;
2649: 
2650: 		/*
2651: 		 * Used for MOTIONSENSE_CMD_PERFORM_CALIB:
2652: 		 * Allow entering/exiting the calibration mode.
2653: 		 */
2654: 		struct __ec_todo_unpacked {
2655: 			uint8_t sensor_num;
2656: 			uint8_t enable;
2657: 		} perform_calib;
2658: 		/*
2659: 		 * Used for MOTIONSENSE_CMD_EC_RATE, MOTIONSENSE_CMD_SENSOR_ODR
2660: 		 * and MOTIONSENSE_CMD_SENSOR_RANGE.
2661: 		 */
2662: 		struct __ec_todo_unpacked {
2663: 			uint8_t sensor_num;
2664: 
2665: 			/* Rounding flag, true for round-up, false for down. */
2666: 			uint8_t roundup;
2667: 
2668: 			uint16_t reserved;
2669: 
2670: 			/* Data to set or EC_MOTION_SENSE_NO_VALUE to read. */
2671: 			int32_t data;
2672: 		} ec_rate, sensor_odr, sensor_range;
2673: 
```

[L2800–L2829](https://github.com/torvalds/linux/blob/36aad21d50d1af3f6954bb0c988257a908202477/include/linux/platform_data/cros_ec_commands.h#L2800-L2829)

```text
2800: struct ec_response_motion_sense {
2801: 	union {
2802: 		/* Used for MOTIONSENSE_CMD_DUMP */
2803: 		struct __ec_todo_unpacked {
2804: 			/* Flags representing the motion sensor module. */
2805: 			uint8_t module_flags;
2806: 
2807: 			/* Number of sensors managed directly by the EC. */
2808: 			uint8_t sensor_count;
2809: 
2810: 			/*
2811: 			 * Sensor data is truncated if response_max is too small
2812: 			 * for holding all the data.
2813: 			 */
2814: 			DECLARE_FLEX_ARRAY(struct ec_response_motion_sensor_data, sensor);
2815: 		} dump;
2816: 
2817: 		/* Used for MOTIONSENSE_CMD_INFO. */
2818: 		struct __ec_todo_unpacked {
2819: 			/* Should be element of enum motionsensor_type. */
2820: 			uint8_t type;
2821: 
2822: 			/* Should be element of enum motionsensor_location. */
2823: 			uint8_t location;
2824: 
2825: 			/* Should be element of enum motionsensor_chip. */
2826: 			uint8_t chip;
2827: 		} info;
2828: 
2829: 		/* Used for MOTIONSENSE_CMD_INFO version 3 */
```

[L2849–L2866](https://github.com/torvalds/linux/blob/36aad21d50d1af3f6954bb0c988257a908202477/include/linux/platform_data/cros_ec_commands.h#L2849-L2866)

```text
2849: 
2850: 		/* Used for MOTIONSENSE_CMD_DATA */
2851: 		struct ec_response_motion_sensor_data data;
2852: 
2853: 		/*
2854: 		 * Used for MOTIONSENSE_CMD_EC_RATE, MOTIONSENSE_CMD_SENSOR_ODR,
2855: 		 * MOTIONSENSE_CMD_SENSOR_RANGE,
2856: 		 * MOTIONSENSE_CMD_KB_WAKE_ANGLE,
2857: 		 * MOTIONSENSE_CMD_FIFO_INT_ENABLE and
2858: 		 * MOTIONSENSE_CMD_SPOOF.
2859: 		 */
2860: 		struct __ec_todo_unpacked {
2861: 			/* Current value of the parameter queried. */
2862: 			int32_t ret;
2863: 		} ec_rate, sensor_odr, sensor_range, kb_wake_angle,
2864: 		  fifo_int_enable, spoof;
2865: 
2866: 		/*
```

## 5. Coachz 实际注册的传感器

[board/coachz/board.c](https://github.com/coreboot/chrome-ec/blob/ad41bf945927e186eaad768a9d51f45ada67b9b9/board/coachz/board.c)  
blob `c1113d4f682760830c6a1a708742df671701df8e`；源文件许可标识：/* Copyright 2020 The ChromiumOS Authors  * Use of this source code is governed by a BSD-style license that can be  * found in the LICENSE file.  */ 

批注：两套表均只有LID_ACCEL/LID_GYRO，类型分别为ACCEL/GYRO。BMI260替换并没有增加光感。这个结论仅对应固定源码，不能证明每块工程机焊接情况或实际运行固件。

[L350–L399](https://github.com/coreboot/chrome-ec/blob/ad41bf945927e186eaad768a9d51f45ada67b9b9/board/coachz/board.c#L350-L399)

```text
350: 
351: struct motion_sensor_t motion_sensors[] = {
352: 	/*
353: 	 * Note: bmi160: supports accelerometer and gyro sensor
354: 	 * Requirement: accelerometer sensor must init before gyro sensor
355: 	 * DO NOT change the order of the following table.
356: 	 */
357: 	[LID_ACCEL] = {
358: 	 .name = "Lid Accel",
359: 	 .active_mask = SENSOR_ACTIVE_S0_S3,
360: 	 .chip = MOTIONSENSE_CHIP_BMI160,
361: 	 .type = MOTIONSENSE_TYPE_ACCEL,
362: 	 .location = MOTIONSENSE_LOC_LID,
363: 	 .drv = &bmi160_drv,
364: 	 .mutex = &g_lid_mutex,
365: 	 .drv_data = &g_bmi160_data,
366: 	 .port = I2C_PORT_SENSOR,
367: 	 .i2c_spi_addr_flags = BMI160_ADDR0_FLAGS,
368: 	 .rot_standard_ref = &lid_standard_ref,
369: 	 .default_range = 4,  /* g, to meet CDD 7.3.1/C-1-4 reqs */
370: 	 .min_frequency = BMI_ACCEL_MIN_FREQ,
371: 	 .max_frequency = BMI_ACCEL_MAX_FREQ,
372: 	 .config = {
373: 		 [SENSOR_CONFIG_EC_S0] = {
374: 			 .odr = 10000 | ROUND_UP_FLAG,
375: 		 },
376: 	 },
377: 	},
378: 	[LID_GYRO] = {
379: 	 .name = "Gyro",
380: 	 .active_mask = SENSOR_ACTIVE_S0_S3,
381: 	 .chip = MOTIONSENSE_CHIP_BMI160,
382: 	 .type = MOTIONSENSE_TYPE_GYRO,
383: 	 .location = MOTIONSENSE_LOC_LID,
384: 	 .drv = &bmi160_drv,
385: 	 .mutex = &g_lid_mutex,
386: 	 .drv_data = &g_bmi160_data,
387: 	 .port = I2C_PORT_SENSOR,
388: 	 .i2c_spi_addr_flags = BMI160_ADDR0_FLAGS,
389: 	 .default_range = 1000, /* dps */
390: 	 .rot_standard_ref = &lid_standard_ref,
391: 	 .min_frequency = BMI_GYRO_MIN_FREQ,
392: 	 .max_frequency = BMI_GYRO_MAX_FREQ,
393: 	},
394: };
395: 
396: struct motion_sensor_t motion_sensors_260[] = {
397: 	/*
398: 	 * Note: bmi260: supports accelerometer and gyro sensor
399: 	 * Requirement: accelerometer sensor must init before gyro sensor
```

[L400–L445](https://github.com/coreboot/chrome-ec/blob/ad41bf945927e186eaad768a9d51f45ada67b9b9/board/coachz/board.c#L400-L445)

```text
400: 	 * DO NOT change the order of the following table.
401: 	 */
402: 	[LID_ACCEL] = {
403: 	 .name = "Lid Accel",
404: 	 .active_mask = SENSOR_ACTIVE_S0_S3,
405: 	 .chip = MOTIONSENSE_CHIP_BMI260,
406: 	 .type = MOTIONSENSE_TYPE_ACCEL,
407: 	 .location = MOTIONSENSE_LOC_LID,
408: 	 .drv = &bmi260_drv,
409: 	 .mutex = &g_lid_mutex,
410: 	 .drv_data = &g_bmi260_data,
411: 	 .port = I2C_PORT_SENSOR,
412: 	 .i2c_spi_addr_flags = BMI260_ADDR0_FLAGS,
413: 	 .rot_standard_ref = &lid_standard_ref,
414: 	 .default_range = 4,  /* g, to meet CDD 7.3.1/C-1-4 reqs */
415: 	 .min_frequency = BMI_ACCEL_MIN_FREQ,
416: 	 .max_frequency = BMI_ACCEL_MAX_FREQ,
417: 	 .config = {
418: 		 [SENSOR_CONFIG_EC_S0] = {
419: 			 .odr = 10000 | ROUND_UP_FLAG,
420: 		 },
421: 	 },
422: 	},
423: 	[LID_GYRO] = {
424: 	 .name = "Gyro",
425: 	 .active_mask = SENSOR_ACTIVE_S0_S3,
426: 	 .chip = MOTIONSENSE_CHIP_BMI260,
427: 	 .type = MOTIONSENSE_TYPE_GYRO,
428: 	 .location = MOTIONSENSE_LOC_LID,
429: 	 .drv = &bmi260_drv,
430: 	 .mutex = &g_lid_mutex,
431: 	 .drv_data = &g_bmi260_data,
432: 	 .port = I2C_PORT_SENSOR,
433: 	 .i2c_spi_addr_flags = BMI260_ADDR0_FLAGS,
434: 	 .default_range = 1000, /* dps */
435: 	 .rot_standard_ref = &lid_standard_ref,
436: 	 .min_frequency = BMI_GYRO_MIN_FREQ,
437: 	 .max_frequency = BMI_GYRO_MAX_FREQ,
438: 	},
439: };
440: 
441: const unsigned int motion_sensor_count = ARRAY_SIZE(motion_sensors);
442: 
443: static void board_detect_motionsensor(void)
444: {
445: 	int val = -1;
```

## 6. 地址宏与枚举并不是一回事

[board/coachz/board.h](https://github.com/coreboot/chrome-ec/blob/ad41bf945927e186eaad768a9d51f45ada67b9b9/board/coachz/board.h)  
blob `5988883cbaa489769d59c2dd332ac898077b3957`；源文件许可标识：/* Copyright 2020 The ChromiumOS Authors  * Use of this source code is governed by a BSD-style license that can be  * found in the LICENSE file.  */ 

批注：OPT3001_I2C_ADDR_FLAGS仅定义地址；同一文件的sensor_id枚举只有两个IMU项目。不能因此直接拿sensor2或猜测私有I2C访问。

[L57–L65](https://github.com/coreboot/chrome-ec/blob/ad41bf945927e186eaad768a9d51f45ada67b9b9/board/coachz/board.h#L57-L65)

```text
57: /* BMI160 Lid accel/gyro */
58: #define CONFIG_ACCELGYRO_BMI160
59: #define CONFIG_ACCELGYRO_BMI160_INT_EVENT \
60: 	TASK_EVENT_MOTION_SENSOR_INTERRUPT(LID_ACCEL)
61: #define OPT3001_I2C_ADDR_FLAGS OPT3001_I2C_ADDR1_FLAGS
62: #define CONFIG_ACCELGYRO_BMI260
63: #define CONFIG_ACCELGYRO_BMI260_INT_EVENT \
64: 	TASK_EVENT_MOTION_SENSOR_INTERRUPT(LID_ACCEL)
65: 
```

[L101–L109](https://github.com/coreboot/chrome-ec/blob/ad41bf945927e186eaad768a9d51f45ada67b9b9/board/coachz/board.h#L101-L109)

```text
101: 
102: /* Motion sensors */
103: enum sensor_id {
104: 	LID_ACCEL = 0,
105: 	LID_GYRO,
106: 	SENSOR_COUNT,
107: };
108: 
109: enum pwm_channel { PWM_CH_DISPLIGHT = 0, PWM_CH_COUNT };
```

## 7. baseboard 的传感器配置

[baseboard/trogdor/baseboard.h](https://github.com/coreboot/chrome-ec/blob/ad41bf945927e186eaad768a9d51f45ada67b9b9/baseboard/trogdor/baseboard.h)  
blob `76bab194910d6cbb9a57e09541bd2805459b9afa`；源文件许可标识：/* Copyright 2020 The ChromiumOS Authors  * Use of this source code is governed by a BSD-style license that can be  * found in the LICENSE file.  */ 

批注：完整核对该固定文件，未找到CONFIG_ALS的启用；此处摘录Sensors配置。缺少启用项是源码支持情况，不能升级成物理不存在的断言。

[L145–L167](https://github.com/coreboot/chrome-ec/blob/ad41bf945927e186eaad768a9d51f45ada67b9b9/baseboard/trogdor/baseboard.h#L145-L167)

```text
145: #define CONFIG_CMD_I2C_SPEED
146: 
147: /* I2C control host command */
148: #define CONFIG_HOSTCMD_I2C_CONTROL
149: 
150: /* RTC */
151: #define CONFIG_CMD_RTC
152: #define CONFIG_HOSTCMD_RTC
153: 
154: /* Sensors */
155: /* Enable sensor fifo, must also define the _SIZE and _THRES */
156: #define CONFIG_ACCEL_FIFO
157: /* FIFO size is a power of 2. */
158: #define CONFIG_ACCEL_FIFO_SIZE 256
159: /* Depends on how fast the AP boots and typical ODRs. */
160: #define CONFIG_ACCEL_FIFO_THRES (CONFIG_ACCEL_FIFO_SIZE / 3)
161: #define CONFIG_CMD_ACCELS
162: #define CONFIG_CMD_ACCEL_INFO
163: 
164: /* PD */
165: #define PD_POWER_SUPPLY_TURN_ON_DELAY 30000 /* us */
166: #define PD_POWER_SUPPLY_TURN_OFF_DELAY 250000 /* us */
167: 
```

## 8. AP侧 proximity 不是光照传感器

[arch/arm64/boot/dts/qcom/sc7180-trogdor.dtsi](https://github.com/torvalds/linux/blob/36aad21d50d1af3f6954bb0c988257a908202477/arch/arm64/boot/dts/qcom/sc7180-trogdor.dtsi)  
blob `0e0bc8543f714e2e471110576ee8dd9a1babdfd3`；源文件许可标识：// SPDX-License-Identifier: (GPL-2.0+ OR MIT) /*  * Google Trogdor device tree source (common between revisions)  *  * Copyright 2019 Google LLC.

批注：AP侧这个节点是semtech,sx9310，label为proximity-wifi，不是ALS。完整核对Trogdor和Coachz DTS后没有找到独立ALS节点；不凭这一点猜型号或开未登记的I2C地址。

[L713–L730](https://github.com/torvalds/linux/blob/36aad21d50d1af3f6954bb0c988257a908202477/arch/arm64/boot/dts/qcom/sc7180-trogdor.dtsi#L713-L730)

```text
713: ap_sar_sensor_i2c: &i2c5 {
714: 	clock-frequency = <400000>;
715: 
716: 	ap_sar_sensor: proximity@28 {
717: 		compatible = "semtech,sx9310";
718: 		reg = <0x28>;
719: 		#io-channel-cells = <1>;
720: 		pinctrl-names = "default";
721: 		pinctrl-0 = <&p_sensor_int_l>;
722: 
723: 		interrupt-parent = <&tlmm>;
724: 		interrupts = <24 IRQ_TYPE_LEVEL_LOW>;
725: 
726: 		vdd-supply = <&pp3300_a>;
727: 		svdd-supply = <&pp1800_prox>;
728: 
729: 		label = "proximity-wifi";
730: 	};
```

## 9. Coachz 的 EC proximity

[arch/arm64/boot/dts/qcom/sc7180-trogdor-coachz.dtsi](https://github.com/torvalds/linux/blob/36aad21d50d1af3f6954bb0c988257a908202477/arch/arm64/boot/dts/qcom/sc7180-trogdor-coachz.dtsi)  
blob `25b17b0425f24e9f4e8c94db300183cf3585b385`；源文件许可标识：// SPDX-License-Identifier: (GPL-2.0+ OR MIT) /*  * Google CoachZ board device tree source  *  * Copyright 2020 Google LLC.

批注：google,cros-ec-mkbp-proximity是状态事件，不是processed lux，不能拿它驱动自动亮度。

[L82–L90](https://github.com/torvalds/linux/blob/36aad21d50d1af3f6954bb0c988257a908202477/arch/arm64/boot/dts/qcom/sc7180-trogdor-coachz.dtsi#L82-L90)

```text
82: &cros_ec {
83: 	cros_ec_proximity: proximity {
84: 		compatible = "google,cros-ec-mkbp-proximity";
85: 		label = "proximity-wifi";
86: 	};
87: };
88: 
89: ap_ts_pen_1v8: &i2c4 {
90: 	status = "okay";
```

## XNU/iPadOS 接口参考与未完成部分

- 已访问 [UniXNU 显示与亮度 wiki](https://unixnu.com/zh/porting/display)：核对亮度编码、服务属性、FadePeriod和硬件/系统验收的区别。页面的KTZ8866控制器不是本板EC PWM，未复制其硬件时序。
- 已访问 [UniXNU 触控与按键 wiki](https://unixnu.com/zh/porting/input) 和其固定链接 [ShengHidInput NativeHidInput.cpp](https://github.com/UniXNU/ShengHidInput/blob/5f3a37cec0c3fc4234b10726b25489a55f0d77ef/NativeHidInput.cpp)：它提供HID报告的集成参考，不构成Coachz ALS/CoreBrightness匹配已经核对的证据。本轮不新增伪造HID服务。
- 阅读当前项目 `platform/coachz-prelink/package/CoachzBacklight.cpp`：背光由原AppleARMBacklight派生，CoreBrightness通过brightness-nits/brightness/FadePeriod提交请求；EC PWM由其独立worker完成。nominal范围20..400nits，最低PWM有已采用的Linux曲线下限。这里复用其OS接口，不重复实现PWM。
- 阅读 `CoachzBacklightStart.cpp`：原类注册和provider依赖已存在，本次未更改该启动流程。
- 查阅Apple公开 [IOHIDEventDriver.cpp](https://github.com/apple-oss-distributions/IOHIDFamily/blob/main/IOHIDFamily/IOHIDEventDriver.cpp) 作为系统HID参考，但没有将它当作本项目iPadOS私有ALS ABI的证明。该链接是访问时的main；本轮没有资格核对或使用其中的原生ALS路径。

原生ALS匹配、事件payload、CoreBrightness订阅/使能请求、系统自动亮度偏好都没有在本轮完成。当前交付是协议客户端、自定义诊断服务和默认关闭的本地策略。若真实设备不存在可访问的LIGHT，后者不会工作，也不会把黑暗值捏造出来。

## 没有运行证据

模型测试仅写源码，没有执行；XNU服务未编译，未纳入构建，未连接设备，未验证lux、PWM、自动调光、系统UI或休眠恢复。

