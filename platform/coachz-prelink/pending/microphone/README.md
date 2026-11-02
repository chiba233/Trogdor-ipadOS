# Coachz microphone — pending only

此目录是尚未接入的麦克风驱动。没有修改活动源码、编译参数、补丁列表或打包脚本；没有编译、执行模型测试、写盘或实机验收。代码完成不代表已经能录音。

硬件链路按主线 Linux 的 **Coachz** 路由实现：ADAU7002 → Primary MI2S SD0 → LPASS WRDMA 全局通道 5。硬件格式为 **48 kHz、双声道 S32_LE**，不是沿用扬声器的 S16/RDMA 配置；转换后送入现有单声道 S16 输入环。

| 文件 | 内容 |
| --- | --- |
| `CoachzMicCore.hpp` | GPIO、独立供电投票、主时钟检查、Primary BCLK、MI2S、WRDMA、中断、启停与采集状态机 |
| `CoachzMicXnu.hpp` | XNU MMIO/DMA 映射、工作锁与 IRQ 锁、ArmCpu 中断绑定；不自动启动 |
| `CoachzMicShared.h` | 独立共享状态区、有效采样区间；内核与 HAL 共用的整数 ABI |
| `CoachzMicHal.h` | 带覆盖检查的 HAL Float32 读取函数；只供 user space 使用 |
| `mic_model_test.cpp` | 主机模型测试源码；未执行、未加入测试列表 |
| `INTEGRATION.md` | 供以后接入使用的内存、电源、服务和 HAL 接口说明 |
| `SOURCES.md` | 固定版本的 Linux 源码证据、批注及 UniXNU/HakiOS 参考 |

实现包含 80 ms 唤醒数据丢弃、真实 DMA 游标、非二次幂环绕、左右声道信号统计、重启/重定位时失效旧数据、超时和 DMA 错误停机。默认选择左声道，调用方可选择右声道；没有未经验证就混合两路信号。

以后接入前仍需由平台建立 **独占的低地址 DMA 预留和 WRDMA 的 SMMU 路由**，并分配新的 RPMh regulator consumer。驱动不会挑选一个“看起来空闲”的物理地址，也不会借用 Wi-Fi/USB 的投票身份。共享 LPASS 电源、PLL、总线和 OSR 时钟由现有音频平台持有；本驱动只操作独立的 Primary 采集资源。

当前边界：没有验证物理麦克风的声道、PCM 对齐、实际采样率、录音权限流程、休眠恢复或全双工表现。发生 worker 超过 64 ms 未服务的情况会停止本轮采集并报告错误，需要调用方明确重新启动。这里没有把缺失的数据补成“已采集”的帧。
