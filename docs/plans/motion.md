# EC motion 适配：加速度计与陀螺仪

已接入：`patches/xnu/coachz-prelink-motion.patch`，`coachz-motion=1` 打开；源码和主机夹具在 [pending/motion](../../platform/coachz-prelink/pending/motion/README.md)。真机上加速度计和陀螺仪以 25 Hz 发布为 3 个 `AppleSPUHIDDevice` 服务。屏幕方向由 `/chosen/display-rotation` 和 rotation 补丁决定，不经过本服务；CoreMotion 是否消费这些数据未验证。

## 实现

沿用已有公共 EC 通道，通过命令版本查询、count-only DUMP、INFO 枚举 BMI160／BMI260 的 lid 加速度计与陀螺仪。独立 IOService 工作循环约每 200 ms 查询 ODR、量程和最新三轴数据，并发布带状态、主机完成时间和轮询序号的完整快照。错误或停用状态不携带旧轴数据。

直接 AP I2C 驱动会重复 EC 的芯片识别和管理；消费 FIFO／共享事件又需要协调现有输入消费者。本阶段选择查询式采样，不改传感器配置，提供后续 motion 前端可复用的底层读数。

源码核对发现 DATA 的 flags 为 0，sensor_num 没有被 handler 回填；不能套用 DUMP 的 PRESENT 位或检查 echo ID。请求必须在 range／ODR 的 data 字段填 -1 才是查询；原始 EC 结构体的独立主机检查确认大小 13、该字段偏移 5。

## 证据

主机：C 协议／采样模型六组行为、可移植头文件的 C++ 语法、候选补丁应用与源码一致性。真机：3 个服务发布，加速度计和陀螺仪读数进入系统日志，陀螺仪 ODR 已打开；主机时间加轮询序号不冒充硬件采样时间。

## 还没做的

CoreMotion／CMMotionManager 是否收到数据未验证；自动旋转不依赖本服务。下一步按真实坐标和系统 ABI 核对 CoreMotion，再考虑 FIFO 和中断。蓝牙、手写笔保持排除。

