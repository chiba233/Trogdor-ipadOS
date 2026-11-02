# GPU 性能修订与交付

本批是后续集成用源码，未构建、写盘或上机。冻结固件及其 addon、信任缓存快照保持不变。主帧 private/UBWC 路径已在实机执行，但存在方向来回变化，像素一致性未通过。一次 Submit Timeout 已被原生探针捕获。新增的选择器读回另触发了同步外部读错误，不能把此前正常的一次计数输出当作稳定性通过。

## 方向变化：缓存的表示与完成写入不一致

同一个 IOSurface 同时有 rotation 3 的竖向纹理和 rotation 0 的横向视图。旧代码在 `coachzTurnedFrame` 建立私有纹理时设置 `turned=YES`；之后 `coachzFrameBind` 可以把同一记录的 texture 替换为横向 seed，却没有清掉 turned。presenter 继续使用旋转 shader，内容被多转一次。另一条路径还会把已有的横向私有纹理直接返回给 rotation 3 请求。这两处都是源码中的确定错误；当次每帧的来源和完成顺序没有记录，因此不把全部闪屏归因于已经独立验证的单一分支。CoreMotion 保持 landscapeLeft 的事件不足以解释该现象。

修订：

- 同一缓存记录的已发布私有纹理固定尺寸和方向，不能在已有视图存活时改换表示。rotation 3 请求只复用相符的竖向表示；rotation 0 seed 明确置 turned 为 NO。
- presenter 再检查 texture 尺寸是否符合 turned，异常组合不进入绘制。`CZ orient` 分开统计 straight、turned、尺寸 mismatch 和交叉方向视图 conflict；conflict 是拒绝错误复用，不要求为 0。
- 记录 render attachment、resolve destination 和 blit destination。命令完成后按写入选择有效内容，避免“最近导入的视图”冒充最后写入的内容；过期完成通知不覆盖较新的记录。
- rotation 3 私有表示也保留对应的正常共享纹理作为写回目标。每个命令缓冲按同一表面的最后写入决定同步方向：private 写入后回共享图像，plain 写入后经同方向 backing 更新 resident private。没有可用 backing 时使 private 无效，显示共享像素；不是继续显示旧 private。
- 完成前的资源持有继续覆盖同步两端。记录只弱持有 backing，避免它反过来永久保活 IOSurface。
- 只读纹理绑定不再触发回写；一个表面在同一命令缓冲内只同步一次。没有跳过实际写入，也没有删除截图及共享像素可见性。

`CZ frame writer private/plain` 和 `CZ mirror` 用于核对上述生命周期。修订尚未实机验收，残影、局部更新、截图和切换快照都仍需观察。

## Submit Timeout：让行没有返回错误，时钟存在倒退

16 ms 的 `coachzWaitForForeground()` 返回 void，到时只结束让行。它不直接把 Timeout 返回给 Submit。不能仅凭 18.2 ms 调用耗时把故障归因于这个门槛。

GPU 的 `ringNow()` 原先使用 `clock_get_system_microtime()`。当前 XNU ARM 实现有整数截断：

```c
/* rtclock 初始化 */
rtclock_sec_divisor = divisor;
rtclock_usec_divisor = divisor / USEC_PER_SEC;
/* absolutetime_to_microtime */
*secs = t64 = abstime / rtclock_sec_divisor;
abstime -= t64 * rtclock_sec_divisor;
*microsecs = (uint32_t)(abstime / rtclock_usec_divisor);
```

实机 counter 是 19,200,000 Hz，所以微秒除数为 19；秒尾的余数最多换出 1,010,526 微秒，进入下一秒后反而变为 1,000,000。旧 ringNow 因而每秒可能倒退约 10.5 ms。实机 gate 的 max `4294957407` 以有符号差值解释为 -9,889 微秒，直接支持此处发生过下溢。

修订后的 ringNow 使用 `absolutetime_to_nanoseconds(mach_absolute_time()) / 1000`。该纳秒实现按完整的每秒除数进行乘除，不使用取整后的“每微秒 tick 数”。只修 GPU 提交时钟，不修改全局 XNU 时钟实现。

当前正常的零 wait_fence Submit 路径中，Timeout 可从 mode 1 的 `useVm → waitAll → wait` 传回；时钟倒退使无符号 elapsed 突然变大，可以制造假超时。该次现场没有内核阶段和 table mode，故仍保留“具体返回分支待下一次现场确认”的边界。

另修正 mode 1 的提交契约：等待旧 VM 空闲超时发生在新任务 IB/WPTR 发布之前，先释放本次临时 job 引用，再返回 Busy，让既有 Turnip 环路重试。不会对可能已经发布的任务盲目重交；ring、映射及真实故障保留原来的错误路径。

新增故障阶段行：input-wait、retire、publish、vm、ring，以及最终 Submit 的完整 wait_fence/timeout_ns。`submit retry ... vm-idle` 保留原始 Timeout，重复重试按二次幂采样。修正时钟与安全的提交前重试是两个独立改动，不把重试当作时钟修复的替代。

## 选择器读回 panic：先取得硬件访问条件

SP selector 0 的 dword offset 为 0xae10，字节偏移为 0x2b840，与实机 FAR 一致。faulting load 来自新增 `profileSample::check`；publisher 锁和 gPmAsleep 不能证明 GX/SP 域仍可访问。这是新增读回造成的回归。

主线的相关保证见 [a6xx_gmu.c](https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/msm/adreno/a6xx_gmu.c)：

- `a6xx_gmu_sysprof_setup` 先取得 active runtime PM 引用并持有 GMU 锁；profiling 使用 PERFCOUNTER OOB vote 阻止 IFPC。
- legacy OOB 的 PERFCOUNTER request/ack/clear 位分别为 17/25/25。`a6xx_gmu_set_oob` 等待确认，而非只设置一个软件标志。
- `a6xx_gmu_gx_is_on` 与 `a6xx_gmu_sptprac_is_on` 读取 CX/GMU 的 SPTPRAC_PWR_CLK_STATUS，检查实际电源和时钟。

修订对应这些条件：publisher 锁排除本驱动的显式 PM 转换；另外取得并持有 PERFCOUNTER OOB，收到 ack 后检查 CX 侧 0x14340 的 A6XX 状态位。GX power/clock off、SP power/clock off 或转换中都不进入 GX 访问，还要求 SP power-on 位有效。OOB 或状态检查失败只输出 power skip，不读选择器。vote 保持到显式休眠，在 slumber 前释放；没有让每次采样主动唤醒已休眠的设备。

A6XX 状态位来自 `a6xx_gmu.xml`：0/1 为 SP 电源转换，2 为 SP power off，3 为 SP power on，4 为 SP clock off，6/7 为 GX power/clock off。不能套用 A7XX 的位定义。所有新增 MMIO 仍为对齐 32 位。

setup 和 sample 都经过同一硬件条件。rateThread 的 governor 时间基准改读 CX 的 19.2 MHz GMU always-on counter；DCVS 时钟测量的 GX 读取也使用受保护快照，跨 epoch 不相减。DCVS 请求与显式 PM 转换串行。resume 失败保持 GX 软件访问阻断，不再清掉 asleep 后继续初始化。

该 OOB/硬件条件修订尚未上机，不能宣称 panic 已修复。冻结固件中已撤下的旧 profile 代码不因本交付而恢复。后续需要观察 `gpu prof power: vote on status ...`；若握手失败，应只出现 OOB timeout/status skip，并且没有 GX 选择器读取。`selector mismatches 0` 只描述读回结果，不替代电源访问条件或实机稳定性。

## host-pages 整帧拷贝

首要形状为 2266×1488、BGRA、row 9072，占所记录 host first payload 的约 97%。9064 像素字节向 16 字节取整为 9072；9072 除以 64 余 48。Mesa `buffer_texture::plane_copy_regions` 保留 BufferLayout 的 row_pitch，计数中的 row 来自该复制布局。

A6xx `fd6_layout_image` 对 explicit linear pitch 进行硬件对齐检查：

```c
fdl_set_pitchalign(layout, layout->pitchalign + 6);
if (explicit_layout) {
    layout->pitch0 = explicit_layout->pitch;
    if (align(layout->pitch0, 1 << layout->pitchalign) != layout->pitch0)
        return false;
}
```

不能通过放宽检查让 9072 直接绑定。对外的 `minimumLinearTextureAlignmentForPixelFormat:` 和 `requiredLinearTextureBytesPerRowForDescriptor:` 现在都至少按 64 字节计算，保留 runtime 原本更大的要求。新分配的同尺寸 BGRA 有机会得到 9088 行距，从源头避免创建不能直接别名的内存；已经分配的旧行距仍走原来的合法复制路径。是否调用这些查询并采用答案由框架决定，不能保证该形状下一轮消失。

配合前述“只同步最后写入，不为只读绑定回写”，这是本批实际减少复制的两处代码改动。既有 host explicit layout/placement 尝试和 `CZ copy/shape`、`L host` 统计继续保留。新行距也不保证消除 placement 拒绝，需看 import 剩余长度和实际 requirements。

## 编译与验收

整批需要重新编译 addon、生成相应信任缓存并编译内核。只取 GPU 时钟、Submit 和电源保护修订时只需编内核；方向缓存和行距能力报告都在 addon，不能靠只编内核获得。

使用原来的 addon/内核构建入口和完整启动参数，不新增参数。补丁以四行上下文生成；上游 checkout 未修改。没有构建、测试套件、写盘或自动上机，也没有提交。

后续采集一轮正常操作即可，不为凑计数反复启动：

- 首先确认无 rateThread abort、无方向来回变化。保留整次内核日志及统一日志，含 power vote/config/skip、CZ orient、frame writer、made/private/shown/plain。
- 若 Submit 仍失败，保留同 PID 最早的 `submit failure/return/retry`、KMD、VK、CB lost，以及 table mode/时钟相关记录。Busy 重试不应被转成 DeviceLost；其他原始错误不要丢弃。
- 初始化后操作约五分钟并记大致起止时间，采集前后 `CZ copy/shape`、`L inp/host`、`CZ mirror`、G/H/J、频率及翻转统计，用累计差比较。
- 检查 9072 的整帧流量是否减少、新行距及 inplace/pitched 是否变化，并反馈局部更新、残影、截图、切换快照及撕裂。约三分钟且含初始化的累计数据不与较长启动直接比较。
- 正常熄屏/亮屏时若实际发生 slumber/resume，检查 vote 的释放/重新取得及新 epoch。没有发生休眠无需重复操作。

原始日志和环境详情不进公开产物。
