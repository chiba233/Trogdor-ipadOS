# GPU 主帧压缩与故障记录

实机结果：诊断和 faults 基线都生效；主帧没有用上压缩（`CZ frames made 0`，表面 row 9216、高 1488，像素占满最后一页，标记被拒绝 35 次）。其余见 [硬件记录](../hardware-tests.md)。

## 当前实现

- 主帧标记的边界与 IOSurface 导入一致：按进程 VM 页大小取整，只使用已映射末页的余量。像素占满整页时拒绝标记，不借用下一页。
- rotation 0 表面先正常导入。presenter 首次见到标记后允许下一次导入提升；提升纹理仅有 RenderTarget 和 ShaderRead，用完成的 blit 保留原像素。之后的导入返回持有 IOSurface、线性 backing 和 countUse 租约的 view。
- 私有帧参与命令缓冲后，末尾向线性 backing 回写一次；同一私有帧去重。这保留截图、快照和其他进程读取 IOSurface 的路径，也会增加一次全帧传输。不能仅凭提升成功宣称性能改善。
- presenter 继续使用既有缩放和双缓冲翻转。翻转锁存、缓冲轮换和等待位置没有改变。
- Darwin KMD 在设备创建时读取 faults 基线。状态值是全局计数加当前客户端 VM 的计数；以后发生变化才失去设备。状态读取错误仍返回错误，没有吞错或重试。
- Vulkan Error 消息同步送入内核 Note 3，避免只保存在 NSLog 或超出固定表的行中。大图像初始化也报告最终布局选择。
- 影子传输统计按 backing 和复制原因分组，按输入与输出的计划像素字节数之和降序。已知 bytes-per-texel 的二维影像计算精确 payload；其他表示、keep 和 merge 计入 unknown，不猜带宽。这不是完成字节数或 DRAM 流量。

## 补丁和构建

`unixnu-gpu-plugin.patch` 只应用于插件构建。`unixnu-gpu-stack.patch` 临时注册运行时复制统计及 Mesa 诊断补丁，构建结束或失败后撤回；Mesa 和运行时也恢复原先的补丁序列。`Adreno740.patch` 增加 Note 3 的即时文本接收，必须和本批 EL0 代码一起集成。

编译入口（从项目根目录运行）：

```sh
python3 -B tools/xnu_gpu_userland.py --build --prime-list build/coachz-gpu-userland/prime-list.txt --wallpaper-root OS_ROOT
python3 -B tools/kernel_source.py --platform coachz-prelink --output build/xnu-source-prelink --ipsw .tools/ipsw/iPad_Pro_HFR_17.7.11_21H461_Restore.ipsw --build
```

`OS_ROOT` 是已有的解包系统目录；cargo 需位于 PATH。已有匹配本批源码的 addon 时可直接复用，只编内核。镜像同时使用本批内核、addon 和对应的信任缓存，继续使用现有打包入口及启动参数，不增加启动参数。构建完成后进入实机验收，不要求全量回归或额外 host fixture。

## 实机验收

本组件提供两个验收项；诊断数字本身不另计组件或验收项。

1. **主帧压缩与像素一致性**：`CZ frames` 中 private 和 shown 持续增加；对应主帧大小的 `IMG` 事件为 `ubwc1`、非线性 tile，不能只看纹理创建计数。`CZ seed` 的 fail 为 0。局部更新、滚动、弹窗、截图和 App 切换快照都保持完整、无旧帧残留。翻转仍须无超时和可见撕裂。任一条件不成立，本项未通过。
2. **渲染稳定性**：在稳定运行后重复相同界面操作和小组件负载，连续至少五分钟；backboardd 不因 DeviceLost 退出，WidgetRenderer 不出现同一 Code 4 后的空地址调用。若再次出现故障，保留最早的 `COACHZ gpu event`、同 PID 的后续事件及内核故障记录，按其原始原因继续定位；不能把“现在有日志”算本项通过。

性能单独比较：避开首次启动的密集初始化，在相同稳定工作负载前后各取一份 `G commit-to-complete`、帧 GPU 直方图、GPU 频率/忙碌率和翻转统计。用累计 total 与 n 的差计算该窗口的均值，用直方图差比较长帧。没有这些数据不判断是否更流畅。

## 需要采集的数据

保留整次启动的内核日志环；若出现渲染进程退出，同时保留同次启动的统一日志归档。原始日志放在被忽略的本地目录，不进仓库。

- 开机初始化结束、界面负载稳定后，记录一份统计；连续约五分钟重复主屏滑动、控制中心开关、App 开关与切换，再记录一份。给出这段操作的大致起止时间，以便从累计计数中分离稳定窗口。
- 完整保留 `CZ frames`、`CZ seed`、`CZ tag`、`CZ mirror`、`CZ pass`、`CZ copy` 和所有 `COACHZ gpu event`。IMG 事件在创建时报告，不能只截取操作末尾的日志。
- 同时保留 `G commit-to-complete`、`H frame-gpu`、GPU 忙碌率/频率、`gpu prof`、翻转次数/超时/等待统计，以及 CP、SMMU、hang、recovery 记录。
- 记录滚动和局部更新是否有残影、截图及 App 切换快照是否正常、撕裂有无变化、流畅度的实际变化。渲染进程若退出，提供发生时间及当时操作；统一日志需要覆盖首次错误前后。

即使主帧未提升或再次 DeviceLost，这份数据也足以定位下一步；无需为凑齐诊断数字反复启动。

## 日志含义与失败表现

| 记录 | 含义 / 失败表现 |
|---|---|
| `CZ frames made / private / shown / plain` | 标记、提升与 presenter 使用；made 为 0 时先核对 tag 边界和候选尺寸，private 为 0 时核对是否重导入及 seed |
| `CZ seed / fail / tag rounded / refused` | 完成的种子、失败次数和标记边界选择；rounded 增加说明逻辑长度原本容不下标记 |
| `CZ tag row / alloc / map / at` | 最近一次候选表面的行距、API 长度、页取整长度和标记偏移；用于核对真实表面，而非从分辨率猜行距 |
| `CZ mirror n / bytes` | 编码的回写数和像素 payload 字节；失败拒绝该命令缓冲并报告 `frame writeback`，不返回陈旧画面冒充成功 |
| `CZ pass ... direct / private / surface / buffer` | 各种目标的百万像素数；不是压缩率 |
| `COACHZ gpu event ... IMG` | Vulkan format、尺寸、usage、flags、最终 tile/UBWC 和格式列表个数；每种大图像布局仅报告一次 |
| `COACHZ gpu event ... VK` | 原始 Vulkan Error，每条消息有递增编号及分段序号，每行不超过 90 字符；包括 submit、释放、映射失败及 faults 变化 |
| `CZ copy b / c / n / in / out / unknown` | 按字节排序的前三组计划传输；n 为次数，in/out 为字节，unknown 为无法精确计字节的次数 |

backing：0 image，1 private buffer，2 mapped buffer，3 host pages，4 aliased host pages，5 IOSurface。

cause：0 back，1 keep，2 merge，3 first，4 beyond held，5 bounded，6 other shadow，7 two ways，8 written，9 within，10 dropped，11 unspecified。

## 风险和后续

末页余量、跨映射标记及私有帧在真实 iPadOS 上仍需验证。完整回写保留 IOSurface 的共享语义，但可能抵消部分压缩收益；后续只能根据复制统计和实际消费者优化，不能直接删掉。最初的 DeviceLost 原因尚未实机确认；faults 基线修正不等于该事件已解决。

下一批先按统计的主要 backing/cause 和创建拒绝原因减少影子传输，再扩大离屏目标的 UBWC 覆盖。提交调度、帧节奏、缓存和硬件扫描缩放仍留在后续批次。

## 源码依据

- `NativeBridgeMetalFrontend.mm` 的 IOSurface 导入将 allocation 按 getpagesize 取整；`NativeGOPPresenter::waitSwap` 将表面映射到 presenter 并传递映射长度。标记使用同一末页边界，不扩大物理分配。[IOSurfaceGetAllocSize API](https://developer.apple.com/documentation/iosurface/iosurfacegetallocsize(_:)) 描述的是 buffer allocation size，不能据此认定返回值已页对齐。
- Mesa `tu_knl_drm_msm.cc`：设备初始化读取 MSM_PARAM_FAULTS；check_status 比较前后值。Darwin 的基线及变化判定沿用此语义。
- `NativeAdrenoUserClient::status` 返回全局与客户端 VM 计数之和；`NativeAdrenoPublish.cpp` 只在无法恢复 GPU 时增加全局故障。
- `NativeBridgeMetalFrontend.mm` 的 command buffer error 将 runtime code 11 在 iOS 映射为 AccessRevoked。Code 4 本身不能区分提交、内存和硬件故障。
- Mesa `vk_device.c::_vk_device_set_lost` 经 `__vk_errorv` 报告原始消息；metal2vulkan 的 device/create debug-utils sink 将其送到 `observe_message`。本批桥接该现有消息，不改变失去设备的处理。
- Mesa `tu_image.cc::tu_image_init` 及 `image_plan.rs` 决定 STORAGE、MUTABLE_FORMAT 和格式列表对 UBWC 的影响。`IMG` 读取布局建立后的结果，不按 Metal storageMode 推断。
- metal2vulkan `EncoderCore::count_shadow_copies`、`ShadowCopy::texels_in/texels_back` 和 `ShadowTarget::Image::texel` 是计划 payload 统计的来源。
