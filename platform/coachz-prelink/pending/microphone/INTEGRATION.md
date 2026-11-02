# 接入说明

接入以 `package/CoachzAudio.cpp` 为准（环 256 KiB，失败后为客户端重启）；本文保留最初的资源划分和挂钩说明。

## 1. 平台资源

### DMA 内存和 SMMU

由 loader/内存规划器预留至少 49152 字节、4 KiB 对齐、4 GiB 以下的独占物理内存，并从可分配 RAM、固件保留区、USB/GPU/其他 DMA 池中排除。没有默认地址。已知扬声器区间 `[0x81200000, 0x81240000)` 会被驱动显式拒绝；其他冲突仍由平台的统一内存规划负责。

可以由 ADT `/chosen/memory-map/CoachzMicDMA` 传入基址和长度，但**添加节点本身不会预留内存**。`prepare()` 的布尔参数是已完成的资源契约，不是实现预留或 SMMU 配置的开关。

主线 LPASS 节点有 `0x1020 / 0x1021 / 0x1032` 三个 IOMMU stream ID。不能从扬声器能播放推断 WRDMA 一定有相同映射。平台必须确认采集通道能够按这段 PA 访问内存，或先建立对应的 identity mapping；当前驱动不改 SMMU。CPU 使用 Normal non-cacheable/write-combine 映射，并执行屏障；不要建立其他 cached alias。DMA 内存和映射保留到本次内核生命周期结束，即使启停失败也不回收。

### 共享 LPASS 和电源

调用 `prepare()` 前，现有音频平台必须完成并持有 LPASS 的 GDSC、CFG/bus/core clocks、PLL、`mclk0`/OSR 和其父时钟。当前扬声器启动代码已经提供这条依赖链；未来若扬声器改为按需关电，麦克风必须成为共享资源的独立消费者。不能在采集期间释放这些资源。

驱动检查共享 PLL 的 lock、enable、L、alpha 和 odd postdivider；只配置 Primary RCG 与 branch，不修改 PLL、Secondary、RDMA1 或扬声器 GPIO。它不是一个能从完全断电的 LPASS 单独启动的电源域驱动。

ADAU7002 的 `IOVDD` 是 `pp1800_l15a`，即共享 RPMh provider 的 `ldoa15`、1800 mV、HPM。注入的回调应采用现有接口：

```cpp
extern "C" int coachz_rpmh_vreg_request(unsigned consumer,
    const char *resource, uint32_t mv, int hpm, int enable);

// consumer 由共享 provider 分配并注册，不能复用 Wi-Fi 或 USB 的身份。
// 成功返回 0。失败的 enable 必须撤回自己的投票；若撤回失败，平台
// 必须记录该资源仍被占用，不能把失败当作完全释放。
static int micSupply(void *context, bool on) {
    const unsigned consumer = *static_cast<const unsigned *>(context);
    const int result = coachz_rpmh_vreg_request(consumer, "ldoa15", 1800, 1, on);
    if (result && on) {
        const int rollback = coachz_rpmh_vreg_request(consumer, "ldoa15", 1800, 1, 0);
        // 集成方必须处理 rollback，不能静默忽略释放失败。
        if (rollback) return rollback;
    }
    return result;
}
```

本次读取的 provider 使用 `voted[3]`、`on[3]` 和 `consumer >= 3` 检查，三个身份已用于 Wi-Fi/USB。**直接传入 3 会失败。**以后接入时，需要在共享 provider 中正式扩展 consumer 存储、范围检查和聚合循环，并审查 enable 失败时的投票回滚。资源表也需容纳新增的 rail。这里没有修改该 provider，更没有独立操作 TCS 绕过其锁。

### 中断归属

`XnuBinding` 在准备阶段调用现有 `armcpu_spi_attach(192, false, ...)`，对应主线 `GIC_SPI 160` level-high。使用 LPAIF host IRQ port 0，通道 5 的三个位；现有轮询扬声器使用另一个 IRQ port。将来若增加其他 host-port-0 使用者，要统一分发 SPI160，并共享 IRQEN 寄存器锁；当前实现不能与另一个独立 SPI160 handler 叠加。

对象、IRQ 锁、映射和上下文必须保持整个内核生命周期，不可放在栈上。现有 ArmCpu 导出没有 detach，故 binding 不提供运行时销毁/卸载。worker mutex 顺序为 worker → IRQ spinlock；ISR 只取 IRQ spinlock，不等待 worker。供电投票和等待时不持有 IRQ spinlock。

## 2. 音频服务挂钩

将来可在现有 `CoachzAudio.cpp` 内包含 `CoachzMicXnu.hpp`，复用同一翻译单元。当前没有添加这个 include，亦没有更改它的编译方式。若项目的 late static-init 处理变化，接入者需核对该对象的构造时机。

准备时：

```cpp
static CoachzMic::XnuBinding microphone; // 内核生命周期对象
// 以下变量必须来自已经落实的板级资源，不提供示例物理地址。
CoachzMicShared *micStatus = reinterpret_cast<CoachzMicShared *>(
    static_cast<uint8_t *>(controlMemory->getBytesNoCopy()) + COACHZ_MIC_CONTROL_OFFSET);
const bool available = microphone.prepare(reservedPa, reservedLength,
    loaderReservationEstablished, wrdmaIdentityMappingEstablished,
    micStatus, static_cast<int16_t *>(inputMemory->getBytesNoCopy()),
    COACHZ_AUDIO_RING_FRAMES, micSupply, &registeredMicConsumer);
```

`CoachzMicShared` 放在现有控制页偏移 1024，原有诊断在 2048。共享扩展有独立 magic/version，不改变现有结构体布局。HAL 与内核都必须识别扩展后才允许录音，旧 HAL 不应被当作真实采集支持。

必须一起完成的接口：

1. 删除原 timer 中的伪输入推进：`control->in_frames = position` 和 `control->in_written = position`。不要再由 playback timer 直接修改 input anchor。
2. 在现有 10 ms worker 中，在更新 playback anchor 后，调用 `microphone.poll(mach_absolute_time(), control->out_anchor_host, ticksPerFrame)`。同一个 worker 串行发布旧 ABI 的输入兼容字段；真实有效区间以 mic 扩展为准。
3. `CoachzAudioStartInput` → `microphone.start(0)`；返回非零必须变为 user-client 错误，不能仍返回成功。`start` 接受后先是 Warming，80 ms 后 worker 才发布 Running。
4. `CoachzAudioStopInput` → `microphone.stop()`。client close、任务死亡、休眠和关机都停止该 client 拥有的 capture lease。已有 `clientClose()` 仅关闭功放，不足以管理麦克风。
5. 输入 ownership 应由服务统一管理。不能让任意一个 client 的 close 停掉另一个 client 的录音，也不能让多个 StartInput 不经仲裁地重置同一个 DMA。这个 pending backend 只管理一个物理采集流。
6. `InputReady` 只在平台资源和扩展准备成功后发布；`InputRunning` 跟随实际 Running，停止和故障时清除。共享 state 的 RMW 要与输出方向串行，避免丢失输出位。
7. worker 发现故障时将其报告给 HAL。64 ms 无服务/无游标进度、非法 CURRENT、XRUN/ERR 都会使本轮采集失败。让上层做明确重启，不要每 10 ms 自动循环启动并掩盖失败。

采集硬件 48 KiB = 6144 stereo S32 frames，两个 3072-frame periods。非二次幂，不能用 bitmask 环绕。它与 65536-frame 的 HAL mono ring 是两块不同的内存，不能把普通 IOBuffer 的高地址直接塞进 32 位 WRDMA。

## 3. HAL 挂钩

现有 HAL 的输入 stream 和 Float32 格式可复用，但当前 StartIO 不加区分地调用 StartInput，IsActive 设置也只返回成功。接入真实麦克风时必须落实实际输入方向的生命周期：

- 跟踪输入 `kAudioStreamPropertyIsActive` 状态和 IO users；仅在输入确实启用且 IO 正在运行时请求 StartInput。StartIO 可能只为了播放，不能把它当作录音请求。
- 输入停用时立即 StopInput；StopIO 最后一个使用者结束和断连时也停止。输入启动失败要向调用方返回错误，已经开始的输出按既有错误策略回滚。
- 初始化不触发采集。目标 iPadOS 是否会对该 HAL 发出 IsActive 切换，需要实机观察；若没有，需要按实际 client/stream 请求建立方向仲裁，不能退回“播放就开麦”。HAL route 不能替代系统录音权限机制。
- HAL 包含 `CoachzMicHal.h`，读取扩展 magic/version，并确认 `ring_frames` 与实际映射长度一致。ReadInput 的 sampleTime 要检查有限、非负且可表示为 uint64_t；再调用 `coachz_mic_read()`，替换当前仅比较 `t < in_written` 的读取。该浮点 helper 不包含进内核头文件。
- `coachz_mic_read()` 检查 `[first,written)` 并在复制后复查 seq，避免环绕覆盖、停止后残留和新 capture epoch 混入。遇到不一致只输出静音，不在实时线程无限重试。可递增现有 `in_misses` 诊断。
- 将输入 latency/safety offset 与 10 ms worker 延迟匹配；启动阶段约 80 ms 的 codec 唤醒会产生尚不可用的输入。保留已经修正的 CoreAudio host-clock 频率逻辑，不重新写它。

## 4. 还要验证

录音回放听感已通过。休眠恢复、与播放同时运行时的长时间稳定性，以及应用停止后 DMA、MICEN、Primary BCLK 和 rail vote 的撤回，没有单独验证。
