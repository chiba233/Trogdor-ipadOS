# GPU 主帧、提交与拷贝修订

最新提供的启动记录已执行到 rotation 3 seed 和实际显示，stage 1 错误为 0；Busy 等待整理也取得 reaped/pauses 的原始记录，没有记录到 DeviceLost。host texture shadow/copy 计数均为 0。局部镜像虽命中 3 次，但只少编码 2.82 MB，镜像仍约 14.56 GB；因此尚不能称为有效解决了写回成本。

本次修订 plain 回退的 backing 生命周期和完成判定，并把 shader 相关保护按表面隔离；不改已发布 private 的方向。它和 GMEM 修订（见 [GPU 路线](gpu.md) 的性能一节）在同一个 addon 里，内核沿用，k148 上机：画面颜色正常、明显更流畅，没有 DeviceLost 或 panic。

k148 开机约 307 秒的最后记录：`CZ seed 16 fail 0`，`CZ frames made 16 private 16 shown 2640 plain 1008`，方向 `straight1328 turned1312 mismatch0 conflict1`；`CZ sync out1517 in1388 miss0 attach1`；`CZ miss tag1008 frame0 invalid0 shape0`，plain 全部是没有有效标记；镜像 2905 次共 39.2 GB，局部写回 4 次、省 3.5 MB；`KMD pressure n32 busy9 reaped8 pauses1`。同样到 307 秒，k147 是提交 3647 次、翻转 1801 次、镜像 627 次，k148 是 7735、3601、2905 次，每次翻转的整帧镜像从 0.35 次升到 0.81 次，原因还没查。

提交数和翻转数同步翻倍（每次翻转约 2 次提交，两次一样），翻倍的是出帧数。多出来的是共享面拷回私有纹理（`in`），它以前总是整帧。k150 的计数显示 plain 写入全是直视图上的 Load pass（`pass L534 C0 D0 rot0 srgb0`），damage 可界定，但几乎每次拷回都因为同一命令把主帧绑成 shader 纹理而被 mask 2 拦成整帧（`guarded222`，`inpart0`）。之后的 addon 让 plain 写入也记录 damage，拷回只拷这块（旋转帧把平面矩形换算成私有坐标 `x = W-y-h, y = x`）；mask 2 只在 usage 带 ShaderWrite 或未知时才拦，只读绑定计入 `CZ damage … ro`。k151 的完整内核日志最后记录 guarded0、mask0、ro2631，sync out1307、in1226、inpart0，damage load4829、full24，mirror 2533 次共 34,162,787,032 字节。commit-to-complete 均值 13662 us，frame-gpu 超过 16 ms 的约占 24%，包含初始化且操作窗口不同。所给结果没有 panic、@crash、DeviceLost 或 KMD fail。

## 整帧回退分类与空记录修订

k151 已经覆盖只读放行和 plain 区域跟踪，1226 次拷回依然全部整帧。现有字段还无法区分混合表示、无界记录、无效起始状态和区域并集覆盖整屏。混合写入目前是待计数确认的原因。

源码中存在一个可达的方向选择问题。Load＋Store 的记录在没有 draw 或 scissor 为空时，damage 为零，但它仍可能作为最后写入选定镜像方向，另一表示上的空记录也会触发整帧混合回退。本批让这类已界定空记录退出方向选择和区域合并。末尾空记录前还有可能写入的记录时，继续回溯到后者。整个表面只有空记录且起始内容有效时，可以完成一次无更新同步。起始状态无效时仍进行完整同步。compute 和未知写入保护继续参与判断。

新增 `CZ infull mix/bound/invalid/guard/wide`，在成功编码的共享→private 整帧镜像上统计原因。

| 字段 | 原因 |
|---|---|
| mix | 同一表面存在另一表示的非空或无界候选写入 |
| bound | 存在无法界定区域的写入记录，包括不满足区域跟踪条件的 pass 或 blit |
| invalid | 起始 private 内容未确认有效 |
| guard | 该表面或命令的未知写入保护触发 |
| wide | 局部同步条件已满足，但 scissor 并集覆盖了整帧 |

原因按独立位统计，可以同时出现，合计无需等于整帧次数。`wide` 只在局部同步条件满足时记录。`CZ damage` 增加 `noop A/B`，分别统计末尾空候选的跳过次数，以及合并区域时另一表示空记录的跳过次数，两者可重叠。

两种方向的整帧同步还会通过独立 Note 3 采样 `MF in/out bits... n... private... plain... empty...`。bits 的 1、2、4、8、16 分别对应 guard、invalid、bound、mix、wide。private/plain 是该表面的候选写入记录数，empty 是已界定空记录数。首次和幂次按各原因分别采样，能保留稀有原因的首次现场。该记录独立于 48 行状态表。

本批只排除了已确认无更新的记录。两种表示都存在可能写入时继续完整同步，后续依据 mix 的实际分布处理按写入顺序的表示切换。下一次需要 `CZ infull`、`noop`、首批 `MF in`、`inpart` 和同窗口镜像字节。像素一致性仍需覆盖局部更新、截图、切换快照和转屏。

此次增量位于 addon 的 frontend，配套信任缓存随 addon 更新。代码检查覆盖补丁应用与撤回的文件字节一致性，编译和实机验证由下一轮构建完成。

## 整帧 plain 绘制的接入跟进

k152 最后记录为 `CZ infull mix0 bound0 invalid0 guard0 wide716`，`CZ sync out782 in716 inpart0 miss0 attach1`，plain pass 为 `L1474 C0 D0 rot0 srgb0`。本次运行没有报告 panic 或 DeviceLost。整帧拷回的原因已收敛到 scissor 并集覆盖整帧，优化方向进入 plain 绘制的 private 接入。

本批包含两处实际路径修订。

- 缓存的 plain 纹理创建 whole view 时，在原 runtime 的范围、格式和 swizzle 验证成功后，可以复用已有效的同方向 private。旧 plain handle 仍供已经记录的命令使用，新 view 的渲染和同步跟踪接入 private。API 的 parent 继续指向原纹理，内部 GPU root 由独立的 resident 引用表示，防止错误地按 plain 计数和回写。该入口覆盖提升前创建、之后持续派生 view 的纹理。
- 重复 IOSurface 导入的 usage 是已有 private 能力的显式子集，且其余完整 descriptor 相同时，可以直接复用 private 和仍存活的 backing。这样省掉先重建共享图像再走 existing-private 的中间步骤。未知 usage、额外能力和其他 descriptor 变化继续走正常导入检查。

新增 plain 写入租约覆盖已编码、尚未提交和正在执行的已跟踪写入。命令完成时释放租约，放弃的写入记录析构时也释放。存在此类写入期间，新 view、重复导入和 seed 复用走 plain 回退，避免拿尚未同步的 private 当作最新像素。持有已完成 encoder 的时间不会延长该租约。

新 view 接入目前覆盖同方向、完整单 mip／单层／单采样、默认 swizzle 的 BGRA／sRGB view，且明确 usage 已被 resident 支持。跨方向接入仍需处理 render 坐标和别名的一致性，本批没有将其算作已完成。`CZ reuse subset/views/busy/cross` 分别记录兼容导入、新 view 命中、未完成写入或提升保护、方向不匹配。view 创建成功后，还需结合实际 private writer、in 和镜像字节判断渲染收益。

### 整帧现场与诊断窗口

`wide` 是命令内 scissor 包围矩形覆盖整帧，不能据此把 Load 改成 DontCare，或假定每个 fragment 都覆盖旧内容。新增 `CZ target same/cross/other/whole/union` 对 coverage-only 的整帧拷回分类。same／cross／other 取最后 plain writer 与 resident 的方向和尺寸关系。whole 表示至少一个 pass 的 scissor 并集已覆盖其整附件，union 表示只有跨 pass 合并后才覆盖；二者都统计编码边界。

独立 Note 3 的 `DP in/target/pass/site` 按各类首次和幂次采样，每次最多保留前两个和最后一个有效 plain 写入记录。记录包含尺寸、rotation、format、usage、load／store、scissor、原导入状态，以及调用模块名和模块内偏移。birth 的 1、2、3、4 分别表示导入时尚无 resident、straight resident 已就绪、已有 turned resident、已有 resident 尚未就绪；0 表示该来源未经过此记录入口。该记录用于直接决定下一处接入分支。

RM、GM、GMb 已整体移到可变 benchmark、DF、cache 和 allocation 行之前，位于前部 CZ 组之后。按当前 CZ 各分组的上限，三行可以完整进入 48 行窗口。取数符号和字段保持原语义。

本批已通过现有工具入口构建 GPU addon 和匹配的信任缓存，产物位于惯例的 `build/coachz-gpu-userland`。编译、4 KiB 签名、信任缓存记录和 addon 中的新功能标记核对通过，既有壁纸资源和 shader 预热列表已一起打包。源码校验覆盖四行上下文补丁的正反应用及文件字节一致性。plain 绘制是否命中 private、像素与转屏一致性、截图／切换快照，以及同窗口的镜像字节和耗时仍需实机验收。

## 此前启动记录

每项取其最后打印；没有固定操作窗口或画面反馈，不能与其他启动的累计耗时直接比较：

| 字段 | 最后记录 |
|---|---|
| made / private / seed / fail | 2 / 2 / 2 / 0 |
| shown / plain | 1071 / 1955 |
| straight / turned / mismatch / conflict | 1069 / 2 / 0 / 1 |
| private writer / plain writer | 2512 / 2474 |
| mirror / bytes / part / skip / save | 1080 / 14,563,388,768 / 3 / 0 / 2,821,792 |
| damage load / full / guarded / mask | 2511 / 2475 / 490 / 2 |
| alias | f44, 2266×1488, row9216, n1960, pitch0, size=room=13,713,408 |
| host shadow / copies / 拒绝 | 全为 0；没有 copy、shape、place 行 |
| commit-to-complete | n6080，累计均值 15164 us，max195 ms，包含初始化 |
| 翻转 | 3001，0 超时，mean9608 us，max17071 us |

局部镜像相对该行整帧策略的编码字节估算只减少约 0.019%，不是 DRAM 或端到端性能收益。load/full 是附件数，guarded 是镜像数，不能相减归因。mask2 表示原命令级主帧 shader 纹理保护实际触发；日志没有保留具体表面关系，不能把 490 次全部说成不必要的保护。

stage 1/shape/plane/alloc/dim 和 seed fail 都为 0；最初有一次 turned private，之后出现一次方向冲突和大量 plain writer。保留跨方向拒绝是必要约束，不能据此重新切换同一记录的 private 方向。旧记录没有 backing-miss 和显示回退细分，1955 次 plain 的准确构成仍未知。

Busy 整理取得 20 条采样记录，例如 `busy17 reaped4 pauses13 us1937`，确认既清理了已完成 fence，也实际进入过无本地 fence 的休眠重试。vm-admit 采样到 256；没有 Submit/KMD/VK 失败、DeviceLost 或 panic。恢复序号到 8、slumber 到 9，所见恢复 status/ring/ME_INIT/wait 全为 0；core accesses dropped asleep 最后为 14。这仍不证明较早方法 5 Timeout 的根因已全部闭环。

## 本次 plain 回退与同步修订

源码可达缺口：private 发布后拒绝跨方向导入，原导入会作为 plain 纹理返回，却未持有规范物理平面。frame 表的 backing 是弱引用；最后一个 private 视图释放后，plain 写入仍可能继续，而其同步不能依赖已过期的弱引用。完成回调此前也仅凭 `frame.backing` 的当时状态猜测是否已同步，既不是本命令的镜像回执，也可能被另一导入替换。该缺口有明确源码依据，但现有日志不足以把每次 plain 显示都归到它。

修订对仍有 resident 的 plain 回退建立当前导入自己的规范平面。物理 Unorm 根纹理可直接作为 backing；旋转或 sRGB 视图另建 raw BGRA8Unorm 平面并由回退视图强持有，避免 sRGB 传输发生颜色转换。不会让一个纹理强持有自己的子视图或自身，也不把 frame 表改为强持有 IOSurface。所有导入范围、对齐和类型检查保留。

镜像优先使用最后写入视图持有的 backing 或其物理根，最后才取 frame 表的弱引用。每个写入记录增加本命令的 coherent 标记，仅在镜像成功编码或已确认无更新时设置；命令成功完成且有该标记，才把 private 标为有效。失败命令仍无效，已有 serial 顺序检查保留。若最后写入缺少 backing，不允许转而选择更早写入的方向；该记录保持无效并计入 miss。

## 按表面隔离未知写入保护

原 mask2/4/8 是整个命令的标志：读取或声明表面 B，就会使同一命令写入 A 的局部镜像也失效。改为按 IOSurface ID 汇总 shader 纹理、surface buffer 和显式资源写入的保护；只影响同一表面的镜像。即使声明只读，目标表面自身的 shader 绑定仍保守保持完整同步。未界定的 compute 保留全命令保护，Clear/DontCare、resolve、plain 或无界写入继续整帧，不取消保护条件。

新增前部记录：

- `CZ sync out/in/miss/attach`：private→共享、共享→private 的镜像次数、最后写入没有 backing 的次数，以及 plain 回退 backing 附加次数。
- `CZ miss tag/frame/invalid/shape`：显示回退时无有效标记/输入、找不到记录、resident 尚未有效、尺寸不符的次数。它们对应 plain 的不同原因；状态行不是原子快照。

这些记录用于区分真正减少写回与同步失败。下一次需要 attach/in 的覆盖、miss=0、invalid 的实际变化、part/save/guarded，以及截图、切换快照和两个方向的画面一致性。不能只看 shown 增加。新增两条前部行，尾部可选摘要会受 48 行上限影响，FR/KMD 独立 Note 3 不依赖该表。

## 此前未 seed 阶段的记录

本轮最后记录为 made 15、private 0、shown 0、seed 0、start 0、shape 1、plain 2508、plain writer 4208。新状态机确实进入镜像，但主帧压缩没有通过。最早 shape 1 时，tag rowpad/refused 都还是 0；按源码顺序，失败位于 `coachzFramePlane()` 创建规范物理平面，尚未进入会先读取 tag 的 `coachzFrameSeed()`。现有记录没有保留它的完整创建错误，不能猜作容量或尺寸原因。

同一片段已出现 mode 1 `vm-admit`，采样序号到 2048，没有 Submit failure、KMD fail 或 DeviceLost。最高记录为 slumber 67、resume 66，所见恢复 status、ring、ME_INIT、wait 都为 0。该路径比此前只触发一次 slumber 的覆盖更完整，但不是长期稳定性结论。另有 MobileSlideShow 的 SIGABRT；统一日志给出的断言为 `!PXPixelSizeAreaIsZero(roundedSize)`，不能把它算作 DeviceLost。

提供的文本本身带有截断标记，阶段计数的最后打印时刻也不同。没有完整操作窗口和画面反馈，不据此宣称流畅度、截图或方向验收通过。

## 后续仍使用旧 addon 的记录

后续约 270 s 的启动仍带旧 addon：状态表保留旧 `wait` 字段，没有新增的 plane/alloc/dim 或 FR 记录。因此这份数据不能验收上面的首次导入、布局与失败捕获修订。它最后记录 made 2、private 0、shown 0、seed 0、start 0、shape 1、plain 1130、plain writer 2314，mirror 仍为 0；主帧提升继续失败，不能把没有方向异常当作 private 路径通过。

这次实际 table mode 为 1，`vm-admit` 采样到 1024；内核和统一日志没有 panic、@crash、KMD fail 或 GPU DeviceLost，没有目标显示进程的退出记录。未出现 slumber/resume，故这次不能增加电源恢复覆盖。没有定义正常操作窗口，G 的累计均值 26713 us 包含开机，不能换算为帧率或跨启动归因。

容量拒绝取得第二种行距的证据，两组均为 f44、1488×2266、why2、offset 0、alignment 64、type 0、bits3：

| row | size | room | 组内次数 |
|---|---:|---:|---:|
| 5952 | 13,499,136 | 13,488,128 | 4 |
| 6144 | 13,934,592 | 13,922,304 | 3 |

两种 size 都恰好等于 row×2268，第二组 room 恰好等于 row×2266，进一步对应 FDL 末尾两行补齐。两组来自各自状态快照，不能把总 placement 130 次全部归给它们。host-first 最后为 4176 次、836,084,255 字节，其中 row5952 整帧组为 54 次、728,310,528 字节；这些累计量仍不等于稳定操作窗口的带宽。

新增 `CZ alias f... 宽×高 row... n... pitch... size... room...`：仅在原有对齐、容量、类型检查通过并且 Vulkan 绑定成功后记录 host 图像。只收集至少 2 MP 的图像，最多保留 32 组，报告 size×次数最大的一组；n 为成功创建次数，pitch 为其中显式行距回退次数。它可以确认整帧真正绑定到原 pages，但不是拷贝流量、节省量或所有成功绑定的总计，未覆盖后续 rebind 和 dma-buf 路径。仅看拒绝次数下降不足以证明该路径成功。

## GOP 等待字段修正

既有 `gop waits ... done ... mean ...` 中的 done 标签错误：NativeGOPPresenter 的统计槽 1 是等待新 swap 的**超时次数**，槽 6 是这些超时等待的总时长；没有可交付的新 swap 时才进入该等待。本次 waits 1376、该槽 215、均值约 500820 us，表示约 500 ms 的空闲超时，不是每帧渲染阻塞 500 ms。

GPU package 改为打印 `timeouts`、`timeout mean` 和请求的超时毫秒数，保留原始统计及其语义；不改 presenter、显示硬件或等待行为。后续启动已打印 `timeouts 158 ... timeout mean 500843 ... req 500 ms`，标签修订已实际进入。

## 此前两次 private 主帧记录

两份后续启动的最后记录如下；每个字段取其最后打印，不构成同一时刻的原子快照，也没有可比较的固定操作窗口。

| 字段 | 第一份 | 第二份 |
|---|---:|---:|
| made / private | 2 / 2 | 3 / 3 |
| seed / fail | 2 / 0 | 3 / 0 |
| shown / plain | 1908 / 944 | 767 / 314 |
| private writer / plain writer | 4406 / 0 | 1521 / 0 |
| straight / turned | 1908 / 0 | 767 / 0 |
| orientation mismatch / conflict | 0 / 0 | 0 / 0 |
| mirror 次数 / 字节 | 1970 / 26,569,847,040 | 782 / 10,547,015,424 |
| host shadow / pitch 拒绝 / placement 拒绝 | 0 / 0 / 0 | 0 / 0 / 0 |
| 翻转次数 / 超时 | 2401 / 0 | 601 / 0 |

这使主帧从“仅分配 private”推进到实际写入、seed 和显示。两个方向计数目前都只覆盖 straight，不能据此验收 turned；mismatch/conflict 为零不能代替画面、截图和切换快照反馈。第二份已有新的 `reuse` 字段，其值为 0，因此提前复用分支仍未取得命中证据。

第二份的 `CZ alias` 为 f44、2266×1488、row9216、n319、pitch0、size=room=13,713,408，证明该形状实际通过 Vulkan 绑定。两份所有 copy 行均没有 b3 host-first，也没有 shape 行；仅剩 b5 的 2～3 次操作，字节未知，不能当作零字节搬运。host shadow 已不再出现，但 mirror 仍搬运上述全帧数据，不能把系统称为零拷贝。

此处 alias 高度 1488 本身已是 4 的倍数，pitch0 表示没有经过显式行距回退。因此不能单靠这些记录验收 2266 行的 unpadded-linear 分支，也不能将旧 row5952 大块流量消失全部归因于该修订。

两次 `vm-admit` 的采样都达到 2048，没有 KMD fail、Submit failure/retry/return、CB lost、VK_ERROR 或 DeviceLost，所给完整内核日志也没有 panic、@crash 或目标显示进程退出。两次均有三次恢复，status、ring、ME_INIT、wait 为 0。这增加了实际压力和恢复覆盖，不是较早方法 5 Timeout 的根因闭环。第二份后两次恢复报告累计 7 次 core accesses dropped asleep，不能略去；它表示访问被现有门控跳过，尚未定位具体调用，不等于硬件 fault 或恢复失败。

## rotation 3 的缺失 backing 已定位

两份都捕获了一次相同的 `FR stage1 ... missing buffer or invalid descriptor`，对应 plane 和 shape 计数均为 1，而 seed fail 为 0。这条路径尚未提交 seed 命令。

源码顺序可以直接解释：

```objc
// 导入成功后，先设置 surface，再调用 coachzTurnedFrame。
texture.surface = (IOSurfaceRef)CFRetain(surface);

// NBTexture 的公开 Metal getter：IOSurface 纹理没有公开 buffer。
- (id<MTLBuffer>)buffer { return _surface ? nil : (id<MTLBuffer>)_buffer; }

// 旧物理平面创建因而总是得到 nil，而非实际持有的 _buffer。
textureOnBuffer(plain.buffer, physical, ...);
```

修订增加内部 `coachzLinearBackingBuffer`，只返回已强持有的 `_buffer`；物理平面通过它取得原有导入。公开 `buffer` 仍对 IOSurface 返回 nil，没有扩大公开 API 或绕过 memory-room、对齐、类型检查。`textureOnBuffer` 的缺失 backing 与无效 descriptor 错误也拆开，后续 FR 不再混写这两种原因。

下一次应首先确认 stage 1 缺失 backing 消失，并取得 turned seed/shown 的实际覆盖。新路径若出现 stage 4/5，按已有独立错误记录继续定位；不能只凭 straight 的成功数量宣称两个方向都通过。

## 提升停滞：分配不等于有效

此前版本存在以下可达闭环，上一批已改为统一 seed；本轮失败发生在 seed 之前：

1. 表面先按 plain 路径完成写入，`coachzCompleteFrameWrites` 使 writeSerial 非零；还没有有效的 private，valid 保持 NO。
2. 后续 rotation 3 导入分配 private，设置 turned=YES 并增加 private，但没有 seed。
3. 返回条件 `frame.valid || !frame.writeSerial` 为假，刚分配的 private 不会返回给渲染方；它也就无法通过 private 写入变成有效。
4. 横向 `coachzFrameBind` 又因已有 turned private 而退出，不进入 seed。presenter 继续退到 plain。

实机还有一条直接现场：`command buffer refused: frame writeback: texture copy`。这是本地编码拒绝，不是 DeviceLost。结合旋转 blit 的 reach 检查，旧写回路径本身也需要修订；不能把未 seed 的可达闭环说成全部实机失败的唯一原因。这个闭环与 private 已分配、seed/private writer/shown 全为零的记录一致。旧代码把 writeSerial 同时当作“是否有完成写入”和“能否首次返回 private”，使两个含义相互阻塞。

修订统一两个方向的提升入口 `coachzFrameSeed`：

- writeSerial 只用于完成顺序，不再作为首次导入的放行条件。
- 新 private 在 seed 命令完成前不发布到 frame.texture；seed 成功后同时设置 texture、turned、valid 和完成序号，才返回 private 视图。
- 已有但无效的同向 private 可以重新 seed。尺寸和方向必须相符，不能把另一种表示替换进已发布的记录。
- 横向候选在严格匹配整屏尺寸、BGRA8、render-target 和物理表面后，首次导入就 seed。纹理可长期复用，不能依赖显示后再次导入才提升；本轮 wait 已累计到 16、start 仍为 0。方向和已发布记录仍不能跨方向替换。
- 完成的 seed 不覆盖已经完成的更新写入；失败或竞争时保留共享路径，并输出拒绝阶段。

private 计数改为首次成功 seed 后发布的记录数；seed 是成功初始化次数，包含重新初始化。它们仍不等于实际 shown 或 UBWC 验收。

## 已有效主帧的重复导入

旧顺序是先导入 IOSurface 的共享纹理；rotation 3 随后再次创建规范物理平面，最后才在 seed 中发现 private 已有效并返回视图。因而有效缓存仍支付线性图像的创建和布局检查，两个方向的可复用路径也不同。

新增 `coachzFrameReuse()`，在映射和 plane 基础检查之后、创建 buffer/共享纹理之前查找已有效的 resident。只有普通单平面、2D、单 mip/layer/sample、整屏尺寸、相同方向，且线性 backing 仍存活、物理尺寸和行距匹配时，才直接创建 private 视图。请求的完整 import descriptor 也须与成功导入时相同，包括 usage、resource options 和 swizzle；配置变化仍走正常导入校验。复用不会再次 seed，不会切换已发布方向；表面 ID 仍经 tag 校验。rotation 3 保留 BGRA8 和 framebuffer-only 限制；普通方向保留 sRGB 视图。

backing 继续由活跃视图强持有，frame 表中仍为弱引用，不因缓存而延长 IOSurface 生命周期。弱引用加载在锁内取得本地强引用；视图创建调用 runtime 时不持有 frame 锁。backing 已释放、提升中或缓存无效时走原导入；普通导入后的 existing-view 路径仍保留，避免 sRGB 读取退化。`frame.texture/backing` 的读取和发布统一放在 frame 锁内，视图继续持有 backing 和 IOSurface use-count。

`CZ seed` 的同一行增加 `reuse` 成功计数，不增加状态表行数。它证明跳过重复导入的分支实际返回了视图，不等于新 seed、shown 或节省字节；收益和像素一致性仍待实机。该修改不会让目前尚未 seed 的主帧凭空命中缓存。

## rotation 3 不能直接按普通 blit 处理

运行时的 `texture_descriptor.rs`、`texture/rotation.rs` 和 `rotated.rs` 明确区分：render load/store 使用旋转后的 frame image，CPU/blit 按纹理自身 extent 和原始行距访问平面。`check_rotated_reach` 会拒绝越过实际 buffer 的 blit。对于物理平面 2266×1488、行距 9216 的表面，不能把 1488×2266 的旋转视图直接当作同尺寸普通 blit 来源或目标。

本批为 turned frame 建立 rotation 0 的物理横向视图，并使用显式 GPU render transfer：

- seed 和 plain 写入后的同步：物理平面 → 竖向 private，采用 rotation 3 的逆变换。
- private 完成后的写回：竖向 private → 物理平面，采用原来的 rotation 3 store 变换。
- 普通横向 private 保留同尺寸 blit；不替换原有 presenter 的变换。

设 private frame 为 W×H，物理平面为 H×W，运行时 `rotated::source_map(3, ...)` 的 store 关系为：

```text
plane(y, W - 1 - x) = private(x, y)
private(x, y) = plane(y, W - 1 - x)
```

在 Metal viewport 坐标下，既有 store vertex 的 UV 为 `(fy, fx)`；新增 inverse vertex 为 `(1-fy, 1-fx)`，两者互逆。新传输 fragment 返回完整 RGBA，不使用 presenter 中强制 alpha=1 的 fragment；nearest sampler 按像素中心取样，避免引入缩放或过滤。

传输使用现有 NMB render API 和已有 AIR 构建入口，没有增加内核 ABI。命令缓冲继续持有两端资源，并按同一表面的最后写入决定同步方向。只读绑定不产生回写。显式旋转传输本身有成本，必须结合 mirror、GPU 耗时及像素一致性判断收益。

## 新增阶段记录

| 字段 | 含义 |
|---|---|
| `CZ promote start` | 已开始的 seed 尝试，两个方向都计入 |
| `busy` | 同一记录正在提升，当前导入退到共享路径 |
| `race` | seed 完成时已有更新写入，拒绝覆盖 |
| `shape` | 物理视图创建失败或尺寸不符合对应方向 |
| `plane / alloc / dim` | 规范物理平面创建、private 分配、两端尺寸不符的独立计数 |
| `CZ seed / fail` | 成功 seed，以及编码或完成失败 |
| `CZ frame writer private / plain` | 实际完成写入的来源，不再把“plain 写入后更新了 private”记作 private writer |
| `viewRefused` | 已有 private 无法创建对应视图 |
| `CZ orient straight / turned` | 实际返回给 presenter 的两种方向 |
| `mismatch / conflict` | 无效尺寸组合，以及拒绝跨方向复用；后者不要求为零 |

创建失败新增独立的 `FR stage` Note 3 分段记录，不依赖前 48 行状态表：stage 1 为物理平面，2 为 private 分配，3 为尺寸，4 为 seed 命令分配或编码，5 为 seed 完成失败。编码和完成失败独立采样，分别保留各自首次错误；stage 5 从该命令缓冲的 error 获取原因。物理平面的错误在创建函数返回处捕获，避免后续调用覆盖；按首次和幂次采样。pipeline、encoder、编码或结束失败同时保留 `frame transfer ...` 问题记录。若仍 shown 为零，阶段计数能区分尚未开始 seed、seed 失败、竞争或视图失败，不再只看分配数量。

## Submit Timeout：证据与本次改动

- `coachzWaitForForeground()` 返回 void。达到 16 ms 门槛后继续提交，不产生 Submit Timeout。本轮最后 gate 记录有 219 次到期、最大等待 32910 us，同时没有 KMD fail 或 DeviceLost，不能再将该门槛直接当成较早故障的原因。
- `metal ... selector 6 status 0xe00002d6` 是最后一次 WaitFence 调用结果，不是方法 5 的提交失败。不同时间的 selector 13 成功记录也不是它的原子现场。
- Darwin 的 `darwin_submit_chunk()` 只重试 `kIOReturnBusy`；其他 Submit 失败会调用 `vk_device_set_lost`。因此能确认接口如何放大错误，但较早方法 5 的 Timeout 没有当时的内核失败阶段和 table mode，准确来源尚未证实。
- 主线 Linux 的 `msm_ioctl_gem_submit()` 将任务交给 DRM scheduler 并返回 fence，A6xx 的 `a6xx_set_pagetable()` 将 `CP_SMMU_TABLE_UPDATE` 放进 ring。它们是硬件顺序与排队设计的依据，没有 XNU 的 `kIOReturnBusy` 接口，不能把本批策略写成照抄 Linux。

本次在 `NativeAdrenoSubmit::submit()` 的 `retireCompleted()` 之后、临时资源持有和 cache publication 之前增加 mode 1 准入检查：不同 VM 且仍有未完成任务时返回 Busy。此时没有发布新任务的 IB/WPTR，fence 仍为 0；Darwin 释放内核调用后重试，保留资源与提交一次的语义。不在持有 publisher 锁时进入正常提交的两秒 `waitAll`。

`useVm()` 还被故障恢复和 replay 调用，因此保留其等待逻辑以及已有的提交前 Timeout→Busy 修订；本次没有将它整体改成非阻塞，也没有将已经发布任务的 Timeout 统一改成可重试。mode 2 的 ring 内页表切换不改。启动打印 `COACHZ gpu submit policy: table mode ...`，实际发生准入压力时打印采样的 `stage vm-admit`。

本轮已确认实际 mode 1、准入分支触发，并继续完成提交；它不再只是未触发的源码修订。但较早 DeviceLost 的准确来源仍未证实，不能把本轮未复现当作全部根因已消失。mode 2 的实现仍未改。再次出现方法 5 错误时，应一起保留第一条 `submit failure/return/retry`、KMD、VK、CB lost，不能拿正常 WaitFence 到期替代。

## 减少影子拷贝：改动和尚缺的归因

主帧提升后直接渲染到 private 图像；turned frame 的 seed 和外部同步改用规范物理平面上的显式 render transfer，避开旋转视图的普通 blit，以及原运行时通过暂存、旋转 kernel 和紧凑 buffer 搬运的路径。同一表面按最后写入方向同步，只读绑定不触发回写。显式 transfer 仍有真实 GPU 成本，并非零拷贝；还需核对 shown、mirror、host-first 字节和截图等外部读者的一致性。

本轮 host-first 最后为 8659 次、1,392,385,243 字节；最大形状 1488×2266、row5952 为 86 次、1,159,901,952 字节。不同时间的最后行不能一一配对，更不能与此前不同负载启动直接比较收益。

新 placement 记录的最大组为 f44、1488×2266、row5952、n4、why2，bind offset 0、alignment 64、size 13,499,136、room 13,488,128、type 0、type bits 3。这里是**容量**拒绝，差 11,008 字节；pitch 和 offset 对齐满足，memory type 也在允许集合。总 placement 为 354 次，而这组只有 4 次，不能把全部 354 次都归到该组。

`Refusal::Placement` 仍包括 `texels.buffer.copied()`；其他拒绝按实际数值继续归因，不能取消 Vulkan 绑定范围或 CPU/GPU 一致性检查。

本批在最终 placement 拒绝处加数值记录，并覆盖 copied-buffer 分支；显式 pitch 回退成功不算最终拒绝。最多保留 64 组，溢出计数单独保留。按“拒绝图像要求或行距×高度，再乘拒绝次数”的估算范围排序，界面报告最大一组的两行 `CZ place`，该排序不是实测传输字节。

| 字段 | 含义 |
|---|---|
| `f / 宽×高 / row / off / n` | Vulkan 格式、尺寸、行距、平面相对 offset 和拒绝次数 |
| `at / align / size / room / t / bits` | 绑定相对 offset、所需对齐、所需字节、可用字节、memory type 与允许位图 |
| `why` | 位 1：offset 对齐；2：容量；4：memory type；8：buffer 已复制，未查询要求；16：默认放置失败后显式布局也被拒绝 |
| `cls` | 0：pointer import；1：fresh pages；2：dma-buf |
| `overflow` | 表满后未分组的拒绝次数 |

`why 8` 时 align/size/bits 为 0，表示没有查询，不能解读为硬件要求为零。本批不猜测 97 次的具体归因，不取消 imported-memory、对齐或 CPU/GPU 一致性检查。剩余 host-first 路径根据下一次明确拒绝原因继续改。

### 显式 host 图像的末尾两行

FDL 的末级线性布局为 GMEM 16×4 block blit 向上补齐高度：`2266 → 2268`。本轮要求恰好为 `5952 × 2268 = 13,499,136`；实际 texels 为 `5952 × 2266 = 13,487,232`，import 的最后一页给出 13,488,128 字节，所以页尾的 896 字节余量放不下额外两行。再次按页取整实际像素长度不会解决。

现有 `turnip-render-mode-for-attachment-memory.patch` 的 `tu_attachment_unpadded()` 已对外部 host/显式线性附件的非整块边缘改用 draw load 和 2D unaligned store，避免整块 GMEM event 越过 texels。跟进为 FDL 参数增加默认关闭的 `unpadded_linear`，仅 Darwin 的显式 host 导入、线性、2D、单 mip、单层、单采样图像设置；FDL 同时要求显式布局和线性 tile mode 才不追加末尾补齐行。普通布局和其他调用仍补齐，UBWC 布局不变。

这是使图像实际布局匹配其按 texel 访问路径，不是把 runtime 的 `placed()` 检查改成忽略超界。默认线性图像仍可能先被拒绝；显式布局重试的要求应降到实际 2266 行后由原检查正常接受。新路径的边缘像素、截图、缓存一致性和计数下降仍待实机。

## 继续减少镜像写回

private 路径走通后，两份记录仍分别编码 26.6 GB 和 10.5 GB 的全帧镜像。当前实现对每个完成写入的表面都取整个 resident extent，即便一个保留旧内容的 pass 只更新小区域。

本批新增写入记录和保守的区域并集。只有已经有效、单 mip/layer/sample、root 尺寸不变的 private 主帧附件，在 Load + Store、无 resolve、level/slice/depth-plane 均为 0 时，才允许缩小镜像。记录独立于可选 draw statistics：三条实际 draw 入口（普通、indexed、patch/indirect patch）都会累积当时的单个或多个 scissor；未设置 scissor 按整附件处理。按各条 scissor 先裁剪再求所有 draw、所有 pass 的包围矩形，不能只取最后设置的矩形。

普通方向用相同 origin/size 做局部 blit。rotation 3 仍用完整 viewport/UV 变换，只将目标 scissor 从 private 的 `(x,y,w,h)` 转成物理平面的 `(y,W-x-w,h,w)`；部分旋转写回的目标 load action 必须为 Load，以保留范围外像素。seed 和整帧传输仍沿用原来的完整路径。

以下情况保持整帧同步：Clear/DontCare、resolve、其他 mip/layer、plain 写入、缓存无效、同一表面出现任一无界记录或其他表示的写入。命令内的 compute 保留全命令整帧保护；已标记主帧的 shader 纹理绑定、IOSurface buffer 和显式资源写入按对应表面保护，不能因为绑定了表面 B 就强制 A 整帧。shader 对资源的任意写入仍不能用 raster scissor 作边界。只缩小现有写回，不延后到 CPU 读取时，也不删除截图/外部读取所依赖的共享像素同步。

写入表、scissor 数组只为符合条件的主帧 pass 分配，普通 pass 不持有区域跟踪数组。纯 Load+Store 且没有 draw 的可界定记录可以跳过镜像；一旦同一命令里还有完整写入，仍恢复整帧。

状态行扩展为：

- `CZ mirror ... part ... skip ... save ...`：局部镜像次数、无更新跳过次数，以及相对原整帧策略少编码的像素字节估算。
- `CZ damage load ... full ... guarded ... mask ...`：允许跟踪的附件、完整记录，以及因命令级保护而采用整帧的镜像次数；mask 位 1 为 compute，2 为主帧 shader texture，4 为 surface buffer，8 为显式资源写入，取累计 OR。

这些是编码记录，不是实测 DRAM 流量或 GPU 时间；命令失败时也不能把尝试的字节当成完成量。增加一条前部状态行，固定 48 行上限会挤掉尾部可选摘要；按字段前缀读取，不依赖 row 编号。必须检查局部更新、移动窗口/滚动、截图、切换快照和两个方向的像素一致性；若 part/save 为零，结合 load/full/guarded/mask 区分实际没有可缩小范围与保护分支占满，不能宣称收益。

依据：[Metal setScissorRect](https://developer.apple.com/documentation/metal/mtlrendercommandencoder/setscissorrect(_:)) 规定 raster fragments 位于 scissor 内；[MTLLoadAction.load](https://developer.apple.com/documentation/metal/mtlloadaction/load) 保留附件原有内容。这里修改 Metal/runtime 的传输边界，没有改硬件寄存器或 Linux 的启动时序。

## Busy 压力下的等待整理

Darwin 的 `darwin_note_fence()` 将提交加入固定长度 inflight 列表，但原实现没有在等待成功后移除已完成项。`darwin_submit_chunk()` 遇 Busy 会继续选列表首项，因而可能反复等待早已完成的历史 fence；没有本连接 fence 时，`sched_yield()` 也可能立即重新运行并再次调用 Submit。两份启动都有实际 vm-admit 压力，这说明该重试路径有现实入口；现有计数不能量化这两种空转各占多少。

本批在该压力分支等待成功之后、所有等待和退役存储锁已释放时，单独锁住连接列表，按原有有符号 fence 序号比较移除已观察完成的前缀。本连接所有队列使用内核 queue 0，原接口已要求其完成前缀有序。只清理下次选择等待对象的列表；不释放 BO，不改变 last_fence，不提前 signal Vulkan sync，也不把未完成任务删除。

列表为空时改为请求 100 us 休眠，然后继续重试；不是新的提交超时或 watchdog，实际调度间隔可能更长。仍只重试 Busy，其他原始提交错误和真实 fence 等待错误继续保留原来的 DeviceLost 处理。内核的准入、16 ms 前台 gate、恢复和已发布任务语义都不改。

成功走出压力后，按首次/幂次保留独立 Note 3：`KMD pressure n... busy... reaped... pauses... us...`。reaped 为这次 chunk 从候选表移除的已完成 fence 数，pauses/us 为无本地 fence 时的休眠次数和实际累计时长，busy 沿用当前 queue-submit record 的累计值。它不是 KMD fail；需要和 vm-admit、Submit 原始错误及同窗口 CPU/提交耗时一起判断收益。较早 DeviceLost 的根因尚未因此闭环。

## 电源与选择器读回的边界

本轮恢复序号到 66，profile epoch 到 67；所见恢复成功且 core accesses dropped asleep 为 0，PERFCOUNTER OOB 为 vote on、状态 0x28。这覆盖了实际发生的恢复和重新配置；不证明从未触发过的寄存器访问安全。

CPU 读取 0xae10 曾在 GMU vote 存在、检查状态正常时产生同步外部读错误。原因未查明，不能断言该寄存器在硬件上不可读，也不能再把 OOB 加状态检查当作已充分证明的安全条件。本批保持现有内核禁用该读回的状态，不恢复 SP/TP 等选择器读回。`selector mismatches 0` 也不能解释为这些选择器全都读回匹配。

## 源码依据

- [Linux msm_gem_submit.c](https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/msm/msm_gem_submit.c)：`msm_ioctl_gem_submit`、`drm_sched_entity_push_job` 和返回 fence。此处用于区分排队和执行，不是 XNU 错误码的来源。
- [Linux a6xx_gpu.c](https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/msm/adreno/a6xx_gpu.c)：`a6xx_set_pagetable`、`CP_SMMU_TABLE_UPDATE`、`a6xx_submit`。本批不改该硬件包序列。
- [UniXNU NativeAdrenoSubmit.cpp](https://github.com/UniXNU/Adreno740/blob/0afbfdc0f3a659cd30b203d460b101c7815cbb16/NativeAdrenoSubmit.cpp)：`submit`、`useVm`、`waitAll`、`recover`；本项目增量在 `platform/coachz-prelink/patches/gpu/Adreno740.patch`。
- [metal2vulkan texture/rotation.rs](https://github.com/steelbrain/metal2vulkan/blob/624bab3918de5b978e1101517abca2b4fa426565/crates/metal2vulkan-runtime/src/texture/rotation.rs)、[rotated.rs](https://github.com/steelbrain/metal2vulkan/blob/624bab3918de5b978e1101517abca2b4fa426565/crates/metal2vulkan-runtime/src/rotated.rs)：`check_rotated_reach` 和 `source_map`，确定旋转传输语义。
- [metal2vulkan texture.rs](https://github.com/steelbrain/metal2vulkan/blob/624bab3918de5b978e1101517abca2b4fa426565/crates/metal2vulkan-runtime/src/texture.rs)、[texture/in_place_image.rs](https://github.com/steelbrain/metal2vulkan/blob/624bab3918de5b978e1101517abca2b4fa426565/crates/metal2vulkan-runtime/src/texture/in_place_image.rs)：copied-buffer 提前拒绝及 `placed` 的 offset、容量、type 检查；本地运行时还叠加既有补丁。
- Darwin 的 `darwin_submit_chunk` 和原始 KMD 错误记录见本项目 `unixnu-gpu-stack.patch` 对应的 Mesa overlay 及当前固定 Mesa 基础源码，不用 Linux 的 errno 代替其实际分支。

## 集成和下一次数据

**功能修订需要重编 addon 和配套信任缓存；可先沿用本轮内核。** 首次导入 seed、有效主帧复用、内部 backing 访问、plain 回退租约、完成回执、按表面保护、局部镜像、Busy 等待整理、FR 记录、显式 host FDL 布局和成功绑定记录都在 addon/其 Mesa 静态依赖内；只编内核或继续冻结 addon 都不会带入。GOP 诊断标签已在后续内核记录中出现，不是本次 addon 修订的前置条件。不要将新 addon 配上旧信任缓存。

addon 跟进只改四行上下文的 `unixnu-gpu-plugin.patch` 和 `unixnu-gpu-stack.patch`；Mesa 修改仍在既有 diagnostics overlay 中，沿用原构建应用和撤回路径。本次不再改 `Adreno740.patch` 和板级 package；既有 `CoachzGpu.cpp` 增量只纠正 GOP 诊断标签。GPU 构建沿用 `tools/xnu_gpu_userland.py` 与 `build/coachz-gpu-userland`，同次生成 addon 和配套信任缓存。运行时 warning 仍在前部；FR 失败通过独立 Note 3 保留，按字段前缀查找，不依赖固定 row 编号。

下一次需要：

1. `CZ promote start`、seed、reuse、实际 private writer、shown、`CZ sync` 和 `CZ miss`，以及方向、残影、截图、切换快照和撕裂；不能只看 private 分配或同尺寸 IMG。若仍 start 0，保留 plane/alloc/dim 和第一组完整 `FR stage`。
2. `gpu submit policy` 的实际 mode、`vm-admit`、`KMD pressure` 或首条 Submit/KMD/VK 错误。没有压力记录只表示没有触发；不能宣称准入分支通过。
3. 两行 `CZ place` 和新增 `CZ alias`，连同 CZ copy/shape、L host、mirror 的 part/skip/save 和 `CZ damage`。确认整帧 alias 的 size≤room，n/pitch 增加，再检查边缘像素、截图与缓存一致性。保留初始化结束后正常操作的大致起止时间及前后计数，避免跨启动负载比较总量。
4. 显式 frame transfer 失败时保留第一条错误及整份内核、统一日志；归档留在本地，不将原始环境标识放入公开产物。

补丁应用/撤回的字节核对、差异检查、GPU 构建和实机验收分别记录。


## 跨方向直接渲染与采样对照

本轮启动记录中，`CZ target cross347 whole347`，`same0`；`CZ infull wide347`，对应的 plain pass 本身覆盖整帧。`private writer799`、`shown614` 已实际发生，继续缩小拷入区域不能消除这组全帧传输。`G mean21906us` 是包含启动的累计值，缺少固定操作窗口，不能据此宣称性能回退或收益。

### 实际优化

- 对已有有效 turned private 主帧的单颜色、单采样 BGRA8 Load/Store pass，延迟选定目标，在完整绑定和程序信息就绪后直接画入 private。限定记录容量为 4096 条调用和 1 MiB inline bytes，超限走现有目标。
- quarter-turn 变体将 clip position `(x,y)` 变为 `(y,-x)`，同步变换 viewport/scissor；片元物理坐标逆变换为 `(y,H-x)`，显式导数对应 `dfdx=dpdy`、`dfdy=-dpdx`。程序变体及缓存依赖显式包含 quarter-turn 标记。
- 检查资源别名、写副作用、状态、sample/quad 和插值语义。选中 pass 结束后通过固定的写入前缀同步共享平面，并封存该前缀，避免后续镜像重复处理。外部读取依然有同步成本，收益需要分别观察拷入、拷出和 GPU 时间。

### 同次启动的 GPU 数值对照

原始与变换后的 VS/FS SPIR-V 哈希共同标识程序族。新程序族至少遇到三次才安排对照，每次启动最多八次。对照执行原 pass 和 quarter-turn pass，以 GPU compute 读取包含边缘的 128×128 网格，共 16384 个像素。正式输出使用原路径结果，对照后从原共享平面恢复 private。

GPU 完成后要求处理行数为 128、差异数为零，才允许同次启动后续匹配程序族走直接渲染。有效差异会关闭本次启动的新直接渲染准入。未完成或局部失败的对照不授予准入。这是抽样一致性证据，不能当成逐像素证明。

| 记录 | 后续可回答的问题 |
|---|---|
| `CZ turn try/run/audit/seals` | 候选、实际直接渲染、对照及同步各走了多少 |
| `CZ turn refuse` | 别名、程序、状态、primitive、等待或容量限制各拒绝多少 |
| `TR program mask/vs/fs/newvs` | 哪个程序及能力限制阻止了准入 |
| `TR pass mode/reason/draws/calls/prep` | 原路径、对照、直接渲染的选择及准备耗时 |
| `TR span wall/held` | 记录 pass 的跨度和 inline bytes，区分准备开销与 GPU 时间 |
| `TR audit valid/rows/diff/first/delta/fs` | 对照是否完整、差异数量、首次坐标、最大 8-bit 通道差及程序 |

下轮同时保留上述记录和 `CZ mirror`、`CZ sync`、`CZ frames`、`G/H/J`。对照是否完成、准入是否命中、拷入是否减少、拷出是否增加、截图/切换快照/方向是否一致，应分别判断。尽量记录初始化结束后的操作起止时间，用同窗口增量比较。

此批需要新 addon 与配套信任缓存，可沿用现有内核。GPU 构建采用惯例入口和输出目录，壁纸与 prime list 在同次打包中携带。

GPU 构建已完成，addon 为 14,332,184 字节、20 项文件，包含 prime list 和两套壁纸；配套信任缓存包含三个签名哈希。前端/AIR 语法与惯例 GPU 完整构建已通过，真机路径命中、像素抽样及性能结果待后续启动记录。


### 入口未命中的跟进

本轮实机中 `CZ turn try0 run0 audit0/0/0`，同时 `CZ target cross400 whole400`、`CZ infull wide400`，新路径未进入。具体入口拒绝原因缺失，尚未确定根因。

颜色零槽的写入记录改为根据输出 attachment 槽位确定，去掉对重复取得的描述符对象地址的依赖。`try` 在 plain IOSurface 进入筛选时计数。新增 `TR entry`、`TR extent`、`TR resident`，按各拒绝原因的首次和幂次采样，包含 pass/纹理尺寸、帧状态、旋转、Load/Store 和格式。

`TR entry mask` 位值依次为 1 缺写入记录、2 不满足 partial、4 格式、8 深度/模板/visibility、16 显式 pass 尺寸、32 多颜色附件、64 缺帧记录、128 帧未有效/未转向/提升中、256 视图旋转、512 private 尺寸。mask 0 表示通过入口，后续继续看 `TR pass` 和 GPU 对照。构建后需要更新 addon 与配套信任缓存。

入口跟进已通过惯例 GPU 构建，addon 为 14,332,720 字节，配套信任缓存含三个签名哈希，壁纸与 prime list 同次携带。入口拒绝原因和直接渲染命中待下一次实机日志确认。


### 多颜色附件的共同 quarter-turn

实机入口记录为 `mask20 partial1 actions1/1 f80`，`tex2266x1488 pass2266x1488`、`flags3 rot0/0 tex1488x2266`。主帧条件满足，阻断来自原先对所有附加颜色附件的统一拒绝。

现在对同尺寸、单采样 2D 的浮点或 UNORM 颜色目标建立同格式的旋转 private 附件，所有颜色槽共同使用 quarter-turn 几何与片元坐标规则。Load 附件在原路径执行前 seed，Clear 保留清除值，附加目标的 DontCare 选择清零。直接路径的 Store 附件在 pass 结束后同步回原目标，DontCare store 不回写。格式或布局不支持时保留原路径并记录具体槽位。

显式 framebuffer-local input attachment 随全部颜色附件共同旋转，保留 subpass load 对当前像素的对应关系；非零偏移/采样插值仍拒绝。GPU 对照对每个 Store 颜色附件各检查 128×128 个样本，独立记录差异，全部完整且零差异才准入。DontCare store 不参与内容验收。程序准入键包含颜色槽位、格式、尺寸和 Load/Store 布局，避免不同 MRT 布局共用一次验收。

- `TR mrt layout` 记录实际附件位图和布局键，`TR mrt color` 记录各槽的格式、尺寸、usage 和 actions。
- `TR pipeline colors/writes` 是管线描述符声明的颜色槽和非零 write mask，不等同于实测像素写入量。
- `TR mrt slot/bad` 的 bad 位值为 1 格式、2 尺寸、4 旋转/类型/采样、8 mip/slice/resolve、16 actions、32 附件别名。入口 mask 的 32 现在表示具体 MRT 布局拒绝。
- `TR audit slot` 按槽记录处理行数、差异、首次坐标和最大通道差；汇总的 valid/good 决定本次启动后续准入。仍是抽样证据。
- `TR mrt traffic seed/in/store/out` 记录附加附件的传输次数及按尺寸和格式估算的逻辑字节，和原有主帧 mirror/sync 分开观察。附加纹理的分配及 seed/store 都有成本，需要结合 G/H/J 的同窗口数据评估。

需要新 addon 与配套信任缓存，可沿用现有内核。真机附件布局、新路径命中和收益待下一次日志确认。

多颜色附件跟进已完成惯例 GPU 编译、签名和打包。addon 为 14,339,768 字节、20 项文件，配套信任缓存含三个签名哈希，prime list 与两套壁纸同次携带。实机准入、各槽像素抽样和传输成本待后续日志。


### 片元补偿参数的绑定接入

实机已进入多附件对照，`audit8/0/0 seals8 run0`，报告 rows 为 0。独立 ST 分段还原出 `a Fragment push field ViewportCompensation in a render pipeline` 的绑定器 invariant。quarter-turn 编译变体请求片元 ViewportCompensation，但 bind plan 的字段准入仍仅接受 Vertex，绘制在提交前遭到拒绝。

绑定器现在对 quarter-turn 变体接受 Fragment 的该字段。实例化仍使用现有的统一字段写入分支，从 draw state 携带与顶点阶段一致的补偿值，片元读取其中的物理平面高度。

无效对照新增 `TR outcome local/metal/report`，区分 EL0 拒绝、运行时错误码和报告映射；原始拒绝原因通过 `TR error part` 分段保留。这样 rows0 时能直接定位提交前拒绝或 GPU 执行/报告问题。

需要更新 addon 与配套信任缓存，可沿用现有内核。后续以有效的逐槽 rows、diff、valid/good，以及 run 和主帧同步量确认路径与收益。

片元绑定接入修订已完成惯例 GPU 编译、签名与打包。addon 为 14,340,992 字节、20 项文件，配套信任缓存含三个签名哈希。真机有效对照与直接渲染命中待后续日志确认。


### 按程序与附件布局隔离回退

实机第一组 `rows128 diff0 valid1 good1` 后已有一次直接渲染。第二组 16384 个采样点中有 3 个差异，最大 8-bit 通道差为 1，首次坐标为 659,503；原全局停用使后续直接渲染停止，最终 `run1 audit2/1/1`。

移除全局停用，继续使用既有程序/附件布局状态表隔离失败项。失败对照中尚待准入的程序键标记 rejected；已通过的程序键保持可用。零差异准入条件继续生效，微小差异的原因由原始数值确认。

- 每个 Store 附件在既有 256-byte 报告内保留前四个异常样本。摘要仍占四个 uint，随后每条占十个 uint，内容为 x、y、参考 RGBA 和 quarter-turn RGBA 的精确 float32 位模式，总占用 176 字节。记录由原差异计数的原子返回值分配，样本顺序按 GPU 到达次序。
- `TR pixel slot/xy/ref/turn` 输出这些位模式，保留细微舍入差，原 rows/diff/first/delta 摘要继续打印。
- 启动内对照预算仍为八次。在预算尚未用满的候选 pass 中，最多保留前 32 个 draw 的管线、viewport、scissor 和混合常量；出现有效差异后，仅输出 scissor 覆盖异常样本的 draw。该范围不能证明三角形实际覆盖了该点，截断通过独立记录标明。
- `TR draw` 关联 draw 序号、异常点和 VS/FS 哈希，`TR blend` 记录各颜色槽的 write mask、混合启用、操作、源/目标因子及常量，`TR shader` 记录片元函数名。绘制记录在失败时格式化输出。

需要更新 addon 与配套信任缓存，可沿用现有内核。下一次同时看 run 是否持续增加、audit 成败数、异常 RGBA 与绘制状态，以及同窗口 mirror/sync/G/H/J。

按程序隔离回退和异常像素现场已通过前端/AIR 语法与惯例 GPU 完整构建。addon 为 14,342,472 字节、20 项文件，配套信任缓存含三个签名哈希。持续直接渲染和异常样本现场待实机日志确认。


### k165 实机记录

启动 45，保留日志覆盖 932 秒。首个 backboardd 的 `try13257 run1073`，八次对照均零差异。随后进程中两次对照分别有七个和两个异常像素，最大通道差为两个和一个 8-bit 单位；异常现场已包含原始像素与绘制、混合状态，最后一个进程 run 为零。

backboardd 两次 SIGABRT 的 asi 为 `Failed to create IOSurfaceAccelerator`，调用栈经 `detach_layer`、`IOMFBDisplay::copy_surface` 和 `create_new_iosa_manager`。前一条匹配失败为 AppleM2ScalerCSCDriver；SpringBoard 和 chronod 随后退出。整次启动没有 panic 或 DeviceLost。提交到完成的累计均值为 73.6 ms，包含界面重启及重新初始化，没有固定操作窗口，不能直接和此前累计均值比较。


### 图层复制路由修订，待实机

针对 `detach_layer` 经 IOSurfaceAccelerator 创建失败退出的现场，采用 QuartzCore 自带的路径选择。backboardd 初始化 Metal 时设置 `CA_DISABLE_DETACHED=1`、`CA_FORCE_COPY_SURFACE_GPU=1`、`CA_FORCE_COPY_SURFACE_MSR=0`，保留图层的常规合成，并为符合条件的表面复制选择 GPU。

这些选项可能已被 QuartzCore 缓存，因此同时写入这份 21H461 镜像的控制字节。写入前要求 CARenderServerStart 相对 QuartzCore 头的位置以及三个分支各三条指令均匹配；环境和缓存值分别记录。已有十秒报告回调继续读取配置，缓存值变化时重设并记录写入次数。作用域为 backboardd 的 arm64 构建。

`SC route` 独立 Note 3 包含 n、match、env、applied、detach、gpu、msr、writes。env/applied 的位 1、2、4 分别对应禁止 detach、选择 GPU copy、关闭强制 MSR。期望 match1 env7 applied7 detach1 gpu1 msr0；首次、配置变化与幂次采样保留记录。env 表示 setenv 成功，applied 表示实际缓存字节读回符合配置。它们不等同于一次表面复制已经执行。

#### 当前镜像的源码级定位依据

输入为原始 SystemOS 中的 arm64 QuartzCore，镜像头为 `0x189429000`，CARenderServerStart 为 `0x189651efc`。只读扫描字符串和选项名称指针表得到下列索引，既有 GENERIC_SHADERS 的 0x55/0x56 与原记录一致。

| 索引 | 名称 | 缓存字节 |
|---|---|---|
| 0x15 | CA_DISABLE_DETACHED | 0x1f491b385 |
| 0xa0 | CA_FORCE_COPY_SURFACE_MSR | 0x1f491b410 |
| 0xa1 | CA_FORCE_COPY_SURFACE_GPU | 0x1f491b411 |

`ca_debug_options` 基址为 `0x1f491b370`。detach 分支读取 0x15，非零时选择现有 reason 0x10 分支。

```text
189566794  b0359da8  adrp x8, 1f491b000
189566798  394e1508  ldrb w8, [x8, 385]
18956679c  35003f08  cbnz w8, 189566f7c
189566f7c             mov w24, 10
```

copy_surface 在 MSR 选项非零时选择其路径；GPU 选项的读取还受调用 flags 的 0xc00 位约束，更早的其他 flags 分支也可能选择 MSR，因此 GPU 选项只适用于可走该分支的复制。

```text
1896e5f4c  d03591a9  adrp x9, 1f491b000
1896e5f50  39504129  ldrb w9, [x9, 410]
1896e5f54  35ffff89  cbnz w9, 1896e5f44
1896e5f58             cbz w8, 1896e5f68
1896e5f5c  d03591a8  adrp x8, 1f491b000
1896e5f60  39504508  ldrb w8, [x8, 411]
1896e5f64  35001d08  cbnz w8, 1896e6304
1896e6304             mov w27, 0
1896e6308             mov w22, 1
```

本次对这两个既有路径选择配置进行接入，沿用 GPU addon 的正常构建和发布机制。参考整体 Metal 接入结构见 [UniXNU GPU 文档](https://unixnu.com/zh/porting/gpu)。

需要新 addon 与配套信任缓存，可沿用现有内核。下一轮核对 SC route 生效读回、相同操作下的 backboardd 存活、IOSurfaceAccelerator/AppleM2ScalerCSCDriver 的错误，以及 direct run、mirror/sync 和分离操作窗口后的性能。此项尚待实机验收。

复制路由修订已完成前端语法检查和惯例 GPU 编译、签名、打包，addon 为 14,342,840 字节、20 项文件，配套信任缓存含三个签名哈希。SC route 的生效读回、崩溃场景覆盖及性能仍待实机确认。


### 渲染链批量优化，待实机

启动 46 的日志覆盖 222 秒：直接渲染 run289、对照 6/4/2，私有呈现 shown609、plain0；mirror 722 次、9,387,286,708 字节。异常样本为少量通道相差一个 8-bit 单位，继续按程序隔离，不放宽零差异准入。SC route 已读到 match1 env7 applied7，窗口内没有界面进程退出；该窗口尚未覆盖此前 932 秒的退出场景。

本批同时调整四处：

1. **同步时机。** 已准入的相邻直渲染 pass 在同一命令缓冲里继续使用同一私有表面，保留各 pass 的 damage，延迟合并拷回。审核 pass、其他表面、普通 render、compute、blit 的 encoder 打开前，以及 commit 前，先同步已挂起的不可变写入前缀。别名、fence 和未知访问仍会拒绝直渲染；跨命令缓冲的发布边界不变。`TR seals` 的 deferred、merged、boundaries 分别回答延迟 pass 数、相邻合并数和实际同步边界数。
2. **临时附件寿命。** 额外 MRT 附件按尺寸、格式及 storage mode 精确复用，仅在实际 GPU 完成后回池；失败命令不回池。最多保留八项，按完整像素容量保守计费且总额不超过 32 MiB。`TR scratch hit/new/drop` 区分复用、创建和未保留项。
3. **丢弃型附件。** 不 Load 且 DontCare Store 的额外附件改为 memoryless，交给现有 runtime 的 transient attachment 与 split-pass 保留逻辑；Load/Store 附件维持私有存储和已有 seed/store。`TR scratch transient` 记录命中次数。依据为固定 metal2vulkan 源码 `texture.rs` 的 Memoryless 分配以及 `render_pass.rs::memoryless_actions` 和 split-pass backing，不引入新的硬件寄存器行为。
4. **编码状态。** 每个 encoder 分别缓存 buffer+offset、texture、sampler、pipeline 和最多 4 KiB 的常量内容；完全相同的绑定不再排队跨桥调用。常量按字节比较；buffer/bytes 共用槽位失效规则，offset 更新缓存。缓存保留对象身份，encoder 结束即失效；资源别名检查和所有权登记仍先执行。`TR bind` 给出调用与各类跳过数，不把跳过次数当帧率收益。

验收一起观察：画面/截图/切换快照是否正常、direct run 与 audit、mirror 字节及 in/out、seals 合并率、scratch 复用和 transient、绑定跳过数，以及固定操作窗口内的 G/H/J 和进程退出。需要重新构建 addon 与配套信任缓存；可沿用现有内核。构建与源码检查不算实机通过，本批暂不提交。

本批已通过前端语法检查、补丁适用检查和正常 GPU 完整构建，构建退出码为 0。标准目录中的 addon 为 14,345,448 字节、20 项文件，配套信任缓存含三个签名哈希；现有壁纸与 prime list 保留。新增公开内容的隐私扫描为零命中。尚无本批实机数据，优化命中率、画面正确性与性能收益均待上述同轮验收。


### 启动 48 的 GPU 结果与后续批量优化

日志环覆盖启动后 876 秒。首个 backboardd 的直渲染为 try9994/run518，审核 8/7/1；失败样本只有一个像素、一个通道单位的差异，继续回退。私有呈现 shown7181/plain2143，mirror7456 次、99,658,642,936 字节，in3126 次因完整覆盖而不能缩区。G 累计均值 11,707 us，包含启动和不同操作，不能当作受控性能比较。MRT 临时附件 hit518/new2/transient520，实际减少了创建；同步 deferred511/merged0/boundaries511，绑定约 108 万次且跳过为零，这两条在此负载未产生预期收益。各计数为各自最后一次打印，不构成同一时间截面的精确比率。

本次 backboardd 因错误释放 SIGABRT，SpringBoard 随后被结束。统一日志的 malloc 信息为 `pointer being freed was not allocated`；崩溃报告调用链为 `find_zone_and_free -> CoreFoundation::__rehashs -> NativeAdrenoMetal`，模块内返回点 0x67c0c 的指令序列与 `coachzFrameTransfer` 中解开 pipeline mutex 后的连续两个 `owners addObject` 相符。该集合也被记录侧的 buffer/texture/attachment 绑定追加，存在记录侧和执行侧并发修改的真实缺口。本次未见 GPU panic、DeviceLost 或 KMD fail；这不等于 EL0 稳定性通过。

新一批沿整条渲染链继续推进：

- **资源保留。** `resourceOwners` 由记录侧独占，新增 `coachzEncodedOwners` 由执行侧独占；mirror、MRT seed/store、审核报告以及内部 seed 转移保留到执行侧集合。两者都在真实完成后释放，避免同一个 NSMutableSet 在两侧同时 rehash。纹理日志需要全集时，仅在 commit 的执行侧合并副本。`TR owners` 记录两侧累计保留数。
- **审核覆盖。** 去掉进程生命周期最多八次的上限，改为最多一个审核在途、相邻启动至少隔 500 ms。仍需零像素差异才准入，失败程序保持隔离；完成或取消都会释放在途名额。`TR admission` 分开记录预热、同程序在途和限速等待。
- **依赖与同步。** 在命令缓冲已有计划直渲染之后，普通 encoder 收集附件及显式绑定的表面身份，再在执行侧打开。无关 render/blit 可保留挂起的私有写；访问该表面、未知 compute、fence、event 和 commit 仍先刷新。大 payload/过长调用序列的中途退回也强制刷新。`TR deps` 区分 kept 和实际刷新类型，`TR capture` 记录额外捕获的 pass/call 与 replay 时间。
- **函数特化。** 新增按库内容 SHA-256、函数名、常量和创建模式区分的进程内缓存。命中返回新的对象及 runtime wrapper，保持 specializedName、标签和常量快照独立，避免重复函数创建；不复用现有按对象序号生成的 bitCodeHash。
- **Render pipeline。** 完整桥接描述符去掉瞬时指针，连同阶段函数内容及有序 linked/private functions 建键。普通、无 reflection/archives 约束的成功结果可复用；每次返回独立 wrapper，颜色状态和 quarter-turn 检查状态不共用可变前端对象。
- **Compute pipeline。** 短函数形式和完整描述符均按函数内容、线程限制、mutability 和 linked functions 建键；保留原来的 zero-rest 重试及 reflection 路径。函数缓存设置 256 项目标，render/compute 缓存各 128 项目标，由 NSCache 在压力下回收。仅成功结果入缓存。`TR cache` 记录三类 hit/miss 和建键耗时；命中也计入管线请求耗时。

缓存的 runtime wrapper 通过已固定 metal2vulkan 对象的 Clone 语义复制；`pipeline/render.rs` 将 RenderPipelineState 明确标为便宜 retain 且 Send+Sync。未新增硬件寄存器访问，也未修改内核构建配置。

同轮验收：错误释放是否再现，审核能否超过八次并继续得到零差异准入，run/shown 覆盖与画面/截图/快照，同窗口 mirror 字节和 deps 刷新原因，函数/render/compute 缓存命中及 T/G/H/J 耗时，长时间存活和 DeviceLost。需要更新 addon 与配套信任缓存，可沿用现有内核；源码检查与构建不代表实机通过，本批暂不提交。

本批前端语法检查、补丁适用检查与正常 GPU 全量构建通过，构建退出码为 0。标准目录 addon 为 14,356,520 字节、20 项文件，配套信任缓存含三个签名哈希；原壁纸和 prime list 保留。公开新增内容的隐私扫描零命中。本批尚无实机结果，未提交。


### 打包导出、跨帧相同比较与完成侧空闲计时，待实机

启动 50 的回写共 1555 次、20,904,054,472 字节，刷新采样 render90/commit165，kept4/merged0。启动 53 覆盖 182 秒，597 次、8,012,454,288 字节，kept0/merged0；审核 11/9/2，错误为少量通道一个 8-bit 单位，未见核心界面退出、GPU panic 或 DeviceLost。后者经历 61 次 slumber/resume，唤醒平均 12,916 us、最长 17,086 us。窗口和操作不同，不作性能因果比较。

commit 的共享页发布不能直接省略，快照及外部读取仍依赖这些完成像素。本批保持该语义，从导出实现及跨帧相同数据的写入开销推进：

- `coachz_export4` 以整数像素坐标读取私有 BGRA8Unorm 纹理，每线程输出四个物理平面像素，恢复原始 8-bit 通道后写成一个对齐的 16 字节块。直向及 90 度旋转均走此路径，末尾不足四个像素逐个写入，保留原 alpha，绝不写入行尾 padding/tag。
- 目标共享 buffer、行距、偏移、范围、格式或矩形不符合条件时沿用原 transfer。输入私有纹理继续使用现有 UBWC；未给它增加 storage-image 写权限。导出无需额外的 render pass、sampler 或线性目标 attachment。
- 比较模式逐块对比目标已有字节，只跳过完全相同的块。GPU 每 256 个完整块抽样一次记录变化率，完成侧用 EWMA 在比较与直接写入之间切换；直接写入模式仍抽样，从而能重新发现静态画面。此模式不改变任何像素，也不推迟跨进程发布。
- `TR export packed/compare/bytes/failed` 记录路径执行及逻辑覆盖量，`sample/changed/ratio/fallback` 记录抽样变化率、策略和回退原因。bytes 是逻辑导出量，抽样也不是精确显存带宽或节省量。收益须结合同窗口 GPU 耗时判断。

审核增加同方向私有目标对照：同一批只读 shader、输入和 draw 序列先画到未旋转的私有 control，再执行原参考和 quarter-turn；MRT control 的 storage 与变体一致。原有 sparse GPU 比较额外记录 `TR identity slot/rows/diff/delta`，用于区分目标布局/GMEM 与旋转产生的差异。它只增加诊断证据，不放宽零差异准入，失败仍隔离。

GPU 电源计时按主线的完成侧语义修订：主线 [msm_gpu.c 的 retire_submit](https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/msm/msm_gpu.c#L654) 在退役时调用 `pm_runtime_mark_last_busy`，活动提交减到零后才 `pm_runtime_put_autosuspend`。原实现仅按提交时间计时，长工作完成时 66 ms 可能已经耗尽。现在在 publisher 锁内观察同一次 shared fence 快照与 issued fence；仍有工作或完成 fence 前进时重置 busy 起点，之后才累计原有 66 ms。`gpu pm: idle` 打印 submit/busy 年龄与两类延长次数。未新增 GX MMIO 或选择器读回，也未关闭 runtime PM。

交付分两部分：addon 与信任缓存包含新导出和 identity 对照；完成侧电源计时位于 `CoachzGpu.cpp`，需要下一次内核构建带入。本批只编译 addon，GPU board 源码只做现有内核参数下的语法检查。实机同时核对导出命中/回退、颜色/alpha/截图、抽样策略与 GPU 耗时、identity/quarter 对照，以及带入内核修订后的 idle/wake 行。尚未实机验收，暂不提交。

本批 AIR 组装、前端语法、GPU board 单文件语法及补丁适用检查通过，正常 GPU 全量构建退出码为 0。标准目录 addon 为 14,364,448 字节、20 项文件，配套信任缓存含三个签名哈希。构建插件包含新 export 与 identity 路径。隐私扫描零命中，尚无本批实机结果；电源源码修订尚待内核构建带入，未提交。

### 启动 56 的结果与同方向私有渲染，待实机

日志环覆盖 178 秒。打包导出执行 packed215/compare215、逻辑覆盖 2,899,754,880 字节，failed0；抽样 719,820 组中变化 113,576 组，约 84.2% 完全相同。这是抽样相同比例，不是带宽节省比例。同方向私有 control 的 identity 对照为零差异，quarter-turn 对照出现 14 个像素、最多一个通道单位的差异；它将问题缩到附加旋转/范围/坐标路径，尚未定位具体指令。run31，mirror361 次、4,868,890,752 字节，反向同步 in148 且全为 wide。G 累计均值 26,447 us，约 70.3% 的 frame-gpu 样本不少于 16 ms；无固定操作窗口，不作版本性能比较。

完成侧空闲计时已随内核实际执行：19 次 slumber/resume 全部 status0，resume 平均 12,414 us、最长 15,882 us；idle 行已记录 pending/progress 延长。窗口内未见 GPU panic、DeviceLost、KMD fail 或核心界面退出，仅覆盖短时间运行。

本批默认新增同方向私有目标路径：

- 物理方向的持久私有 BGRA8 目标使用原 shader、viewport、scissor 和原 pass 范围，单独建立准入 key 域。身份路径不再要求 quarter-turn 的 quad/插值或三角形限制；仍拒绝内存写副作用、depth/stencil、tessellation、多采样、未知访问等未覆盖条件。实际 draw 参考审核仍须零差异，失败程序隔离。
- 每个表面保留内部同方向目标，并限制同一命令队列复用。目标有效时不再从规范旋转纹理反向 seed；普通写、其他规范写、重新 seed、非 coherent 写或失败完成使其失效，再次使用前恢复内容。
- pass 结束后在 GPU 内将同方向目标更新到既有旋转私有表示，保留外部别名及呈现契约。共享页仍按现有完成边界发布；打包导出可直接读取同方向目标。额外 MRT 的 Load/Store 使用同方向纹理复制，原有临时池及 memoryless 规则继续使用。
- 前八次实际表示转换增加稀疏零差异检查；无效结果或差异会禁用后续新路径并使相关完成状态退回共享像素。它验证内部复制的方向/格式映射，不替代 shader 参考审核或画面验收。
- `TR physical mode/seed/reuse/rotate` 与 `bytes seed/rotate` 分开记录反向初始化、持久复用和额外规范转换成本；bytes 为逻辑像素量。`verify/bad/missing/disabled` 及逐次 `check valid/rows/diff/delta` 检查转换。新增私有到私有复制可能抵消直接渲染收益，须用同一操作窗口的 G/H/J 和命中率判断总效果，不能只看 run 增加。

固定 metal2vulkan 的 `RenderPipelineState` 原阶段代码与接口是身份准入依据；没有新增硬件 MMIO 或改动内核参数。调试开关 `NATIVE_METAL_COACHZ_PHYSICAL=0` 保留旧 quarter-turn 路径，正常构建参数及输出位置不变。

交付需要新 addon 与配套信任缓存，可沿用启动 56 已带入完成侧空闲计时的内核。本批尚未实机，重点一起核对：新准入和 run、identity/实际审核、physical 检查、seed/reuse/rotate 成本、反向同步及 mirror、颜色/方向/截图/快照，以及固定窗口性能和长期存活。不提交尚未实机确认的改动。

本批前端语法、补丁适用及隐私检查通过，正常 GPU 全量构建退出码为 0。标准目录 addon 为 14,367,888 字节、20 项文件，配套信任缓存含三个签名哈希。需要同时替换两份产物，可沿用当前内核；尚未实机验收，未提交。

### 启动 58：同方向私有渲染的阶段性结果

内核日志覆盖 182 秒、无丢失。最终表计为 try436/run150、audit15/15/0；八次实际表示转换检查均 valid1、rows128、diff0、delta0，verify8/bad0/missing0/disabled0。原 shader 的同方向私有渲染已实际执行并通过现有稀疏审核；它不等同于完整画面、截图和切换快照全部验收。

最近一次事件计数为 seed38/reuse104/rotate127，逻辑初始化 512,514,816 字节、规范表示更新 1,712,878,464 字节。表计同步 out288/in58，其中 inpart1、wide57；mirror346 次、4,626,728,392 字节，part3，区域缩减累计 39,853,880 字节。事件与表计各自取最后一次打印，采样时刻不同，不能相加或强行对齐。

打包导出 packed257/compare257，逻辑覆盖 3,439,824,256 字节、failed0，三类 fallback 都为零；抽样 853,884 组、变化 188,929 组，不把相同比例当实际带宽节省。同步 deferred127/merged2/boundaries125，无关依赖 kept14，render 刷新 53 次、commit 刷新 72 次；逐 pass 更新旋转表示与低合并率仍需继续优化。

G 累计 n822、均值 27,791 us、最大 176 ms；frame-gpu 818 个样本中 557 个不少于 16 ms，约 68.1%。没有固定操作窗口，不能据此认定版本变快或变慢。内核日志未见 panic、KMD fail、DeviceLost、核心界面进程退出或 GPU IRQ 错误；现有统一日志覆盖的界面活动窗口也未筛到相关 GPU 错误，仅为短窗口证据。最后记录一次 slumber，status0；此前 19 次恢复的证据见上一节。

本次保存已构建且已执行的新路径、打包导出、资源保留分离、缓存及诊断、完成侧空闲计时。直渲染覆盖与现有零差异采样成立，长期稳定性、完整像素一致性及性能收益仍保留上述边界；不以构建成功替代实机结果。

### 按表面合并发布和导出矩形边缘，待实机

同步依据为 Linux 主线 [`msm_gem_submit.c`](https://github.com/torvalds/linux/blob/d164c7d754f0e1f020298366ea92430e62476089/drivers/gpu/drm/msm/msm_gem_submit.c) 的 `submit_fence_sync`、`submit_attach_object_fences` 和提交持有引用，以及 [`dma-resv.h`](https://github.com/torvalds/linux/blob/d164c7d754f0e1f020298366ea92430e62476089/include/linux/dma-resv.h) 的对象读写依赖语义。主线按实际访问对象组织依赖，并保留完成 fence；这里将同样的约束接到既有 Metal 表面表示。旋转坐标与原 shader 保持原实现。

- 每个命令缓冲按表面保存独立的不可变写入前缀，并记录原生私有、旋转私有和共享页的编码版本。同一原生目标连续写入只前进原生版本；实际读取旋转/共享表示、未知访问、共享事件或 commit 才转换并发布。无关表面可以穿插，damage 并集保留到该表面的发布边界。审核和表示切换仍先发布旧前缀。
- 发布记录强持有本次编码实际使用的两个私有表示，核对其队列和 resident 身份；旧命令完成回调对全局缓存提示的失效不会替代本命令的编码回执。资源仍保留至实际 GPU 完成，成功完成后的 coherent 回执才使缓存有效。
- 合并限于同一命令缓冲。提交前仍完成旋转表示和共享页发布，保持跨进程、CPU 读取及截图的完成边界；没有跨命令缓冲省略发布。每个 pass 本来就在独立命令缓冲的负载不会因此减少旋转次数。
- `coachz_export4` 将工作组起点向下对齐到四像素；首尾组逐 lane 检查实际矩形，只有完整组才做对齐的 16 字节访问。支持任意列起点和一至三像素窄区域，矩形外像素与行尾 padding/tag 保持。格式、buffer 偏移/行距对齐、容量和范围限制仍生效。没有抽到完整组的窄导出不更新变化率估计。

新增记录：`TR versions writes/rotate/merged/encode` 区分原生写入、实际旋转转换、被合并的更新和转换编码耗时；`encode` 是主机编码耗时，不是 GPU 执行时间。`TR version native/canonical/shared` 是一次发布的编码版本，不是 GPU 完成声明；发布必须取得全部写入的 coherent 回执。`TR publish size` 给出每个边界含 0、1、2、3–4、5+ 个原生 pass 的分布及边界原因，用于区分没有合并机会和合并未生效。`TR export edges` 统计矩形边缘路径，`why b/s/r/l/range` 分别区分 backing、形状/格式、矩形、布局和容量拒绝。

同轮验收：

1. 实际直渲染和内部转换继续零差异，画面方向、颜色/alpha、局部重绘、截图与切换快照正常；缺失或失败的转换报告保持可见。
2. 在同一操作窗口内关联 `TR versions`、`TR seals`、`CZ mirror/sync` 和有效呈现。存在可合并写入时，应看到 merged 增长且 rotate 少于 writes；全是 commit 边界或 merged 为零时不宣称减少了转换。
3. 非四像素对齐 damage 命中 edges，矩形/布局拒绝可分别归因；检查窄更新及边缘像素，不能将逻辑覆盖量当 DRAM 节省量。
4. 结合 `gpu stamps` 的排队/执行、呈现间隔和提交到完成时间评估净收益，保留原有恢复修复并观察实际故障。构建和主机检查不代替实机结果。

交付需要同次构建的 addon 和信任缓存，可沿用当前内核。修改只在插件 overlay 内，使用标准 GPU 构建入口及原输出目录，壁纸和 prime list 同次携带；未实机验收，不提交。

本批前端语法、AIR 组装、补丁适用和隐私检查通过。主机直接执行导出 AIR 函数体的 2810 组矩形检查保留了范围外像素、行尾和前后 guard；纹理读取用坐标桩代替。实际发布函数的主机调用覆盖 A/B/A 合并、选择性读取、事件/提交、表示切换及失败回执，GPU 复制用桩代替。正常 GPU 完整构建退出码为 0，标准 addon 含 20 项文件、配套信任缓存含三个签名哈希；这些检查不建立实机像素、性能或长期稳定性结论。

### 上游差异的接入顺序

核对版本为 UniXNU 主仓库 `5dc00d201496a1e7a72d1f5db23f041eb94ace1c`、AdrenoMetal `6d7f73437087c57ee54a41adceb1735daaea405e` 及 translator `963f0e64f103d2e682cd0cc454a8fe50f52c8cbb`。AdrenoMetal 与当前 pin 的提交历史有分叉，因此按实际源码差异判断，不能把 compare 中的全部提交都算作尚未接入。

| 更新 | 当前源码与接入判断 |
|---|---|
| [编码线程继承渲染线程调度策略](https://github.com/UniXNU/AdrenoMetal/commit/241f3557234d) | 已接入；在首个渲染线程自身读取策略快照，由编码线程继承非 timeshare 策略，记录读取、设置及实际策略结果。收益待同窗口测量 |
| [spare 图像块供 pool 补空间](https://github.com/UniXNU/AdrenoMetal/commit/0f17e741b37c)及后台释放 | 已回移 spare 补空间及 [有界后台释放](https://github.com/UniXNU/AdrenoMetal/commit/59aabad6808b)，接入现有 room worker；保留当前 translator pin、导入所有者、WC 缓存维护和完成顺序。没有整体升级到新运行时的独立 free thread |
| 共享 shader cache、memoryless→GMEM 与颜色顺序 | 当前源码或 overlay 已包含，不能再次记作新收益 |
| lowering 线程、commit 直接提交、后台管线预热 | 上游以 `sheng-metal-flow` 实验开关控制，默认关闭。[上游说明](https://unixnu.com/zh/drivers/adreno-metal#缓存和优先级)记录未测出帧率提升及一次停止出帧，不作为默认性能修复启用 |
| A740 双 ring 调度 | 需要目标代际的上下文和抢占协议。Linux 主线 `d164c7d` 已有 [A6xx 抢占实现](https://github.com/torvalds/linux/blob/d164c7d754f0e1f020298366ea92430e62476089/drivers/gpu/drm/msm/adreno/a6xx_preempt.c)，但 [A618 条目](https://github.com/torvalds/linux/blob/d164c7d754f0e1f020298366ea92430e62476089/drivers/gpu/drm/msm/adreno/a6xx_catalog.c#L847)没有 PREEMPTION quirk；默认门限仍选择单环。强制启用的源码入口不构成 A618 已验证支持 |

### 编码调度、spare 补空间与后台释放，待实机

这批在前述发布/导出修订之上接入两项上游实现，仍通过现有插件及 stack overlay 构建。

- 编码线程保留交互 QoS，另在首次遇到渲染线程时读取该线程自己的调度策略和优先级，以 release/acquire 快照传给编码线程。非 timeshare 策略只尝试继承一次；读取或设置失败不改变原有编码顺序。快照不保留可能退出的线程句柄。`TR encode sched` 一次记录参考策略/优先级、读取结果、设置结果和编码线程实际读取结果；set=-1 表示未尝试设置。帧 trace 的 type 14 记录每批入队、开始、结束、调用数、剩余队列深度和线程 CPU 时间，区分排队与实际执行。
- 已空出的自有图像块可供 dedicated 或 pooled 图像复用；room keeper 在新分配前先取相同内存类型、尺寸足够的最小 spare。池在锁内已恢复空间时不再添块。spare 仍按十秒老化，并响应内存压力和分配失败；不引入短时间反复分配的字节上限。
- 已无 placement、无待执行 GPU 使用、无导入页或外部所有者回调的大块自有内存，可交给现有 room worker 在分配器锁外物理释放。排队及正在释放的内存总计最多 128 MiB；小于 256 KiB、队列已满、导入内存或带所有者回调的释放保留原路径。worker 最长每 250 ms 收取一次，也在补空间前收取；所有释放都经过既有进程退出保护。设备由 worker 的强引用保持到本次释放结束，销毁前停止 worker 并释放余下队列。
- 分配失败重试及内存压力会同步取走尚在队列中的对应 backing，并修正计数，不必等下一次 worker 轮询；已经被 worker 取走的内存仍计入上限，直到真正释放。GPU in-flight parking 分支与导入所有者的释放契约保持原有顺序。

运行时 stage 40–52 记录内存申请、池空间、句柄、复用来源、spare 保留、排队、物理释放及队列满回退；复用来源 1/2/3 分别为池内 placement、spare placement、spare 补池。stage 42/43/48/49 的大小采用向上取整的 64 KiB 单位，饱和于 65535，适配既有 16 位记录参数；不将其误读为 KiB。句柄由 stage 46/58/47/59 按低至高各 16 位拼回，避免记录参数截断。stage 53–57 分开记录完成状态收集、调用方完成回调、保留对象释放与完成门打开。记录仍由既有诊断开关控制。

同轮实机验收需要实际调度结果、type 14 的排队/执行、内存复用与释放阶段，以及同操作窗口的有效呈现和 GPU 排队/执行时间。检查启动、转屏、截图、切换快照、内存压力与长期运行；继承成功、减少分配或主机编译通过均不单独构成帧率收益。回移会改变既有构建入口生成的运行时内容指纹，首次启动包含缓存重建；性能比较需保持相同预热状态。交付仍为标准目录同次生成的 addon 和配套信任缓存，保留壁纸及 prime list。未实机验收，不提交。

本批前端语法、严格记录前缀重放、补丁正反应用字节一致性与隐私检查通过。正常 GPU 完整构建退出码为 0；标准目录 addon 为 12,256,200 字节、20 项文件，配套信任缓存含三个匹配签名哈希。已核对 addon 内二进制、prime list、两组壁纸及新功能标记，插件签名验证通过，临时 overlay 撤回后的上游 checkout 干净。尚未实机验收，未提交。

### 启动挂起、CPU 页表恢复重放与复位前记录

最新实机日志记录新调度路径生效，参考与实际均为策略 4、优先级 54。GPU 随后停在 fence 25，最后发出 27；冷启动、ring restart 和 ME_INIT 返回成功，恢复却停在 replay VM 阶段，状态为 BadArgument，重放数为零。之后 WaitFence 返回 DeviceLost，backboardd 在 Turnip 顶点缓冲绑定路径触发 SIGSEGV；日志仍持续到启动后 407 秒。这批未通过实机验收，不能将构建及主机测试结果当作性能收益。

同次启动的帧 trace 共 1638 条。最后带结束时间戳的是序号 24 的 render pass，随后的 1488×2266 pass（序号 25）连开始时间戳都未写出；pass 序号与 KMD fence 是两个独立编号。当前范围只能收窄至两者之间的收尾、转换或同步命令，不能认定后一 pass 的 shader 是原因；相同 fragment shader 哈希此前已有两次完成记录。runtime 记录没有 spare 补池或 spare placement，三次后台物理释放均发生在第一次 fence 等待超时约 516 ms 后，不能作为首次停住的起因。这不排除其他资源生命周期错误。启动参数未启用 `log-gpu=1`，没有本次按进程生成的管线日志；旧 `pipelines.log` 不参与本次判断。

恢复错误已通过实际 Adreno740 提交代码和软件 SMMU/CP peer 复现。ring 重置到挂起 fence 后，`useVm` 的 `waitAll` 把尚未重放的后续 job 当成 ring 已发出的 fence 等待，超出重置后的序号，触发 BadArgument 并 poison 整个提交器。mode 1 的重放切表现在只等待实际 ring idle，然后写入 TTBR0；原队列中后续 BO 的引用保留到真实完成，正常提交的切表等待逻辑保持。依据为主线 `msm_gpu.c::recover_worker` 的复位、退役故障提交、再重发剩余提交顺序；不以旧队列记录替代复位后实际发出的工作。

首次 GPU 挂起的触发原因尚未定位。新增可选捕获回调在故障 job 仍持有 IB、尚未释放和复位时执行，板级最多保存两份现场：IRQ 原始错误、现有 CP 寄存器及 SQE/ROQ 调试 bank、当前 SMMU 状态、提交进程与 fence、正在执行的 IB1/IB2 窗口及实际提交 IB。IB 总量上限为 64 KiB，其中两个 CP 窗口各最多 8 KiB，截断与读取失败均显式记录；没有增加 SP 选择器读回。捕获写入固定缓冲，慢速打印由报告线程在 publisher 锁外执行，记录捕获耗时，避免日志输出阻塞恢复。静态现场不再依赖恢复十秒后才读到的已清空寄存器，也不再用最初八次提交代替挂起 job。

捕获完整性复核又补齐三个实际缺口：找不到 job 或 fence 读取失败时仍保存硬件、ring 和 fence 页现场；IB 读取先核对 BO 映射范围，只统计真正读取成功的字数，避免 `peekWords` 将映射末尾之外补零却返回成功；新的输出循环保留零值和全部捕获字，不再经过旧 dumper 的 128 行上限。原始提交包最多 128 个，ring 和 memptrs 分别最多保存 32 KiB、4 KiB，打印实际量与总量；IB 分开标记单窗口上限、总容量上限、映射末尾截断和读取状态，并记录映射句柄、基址、大小及权限。还保存同一时刻的队列上下文、完成/提交序号、恢复阶段、电源门控丢弃计数和单调时钟，启动时打印捕获已安装标记。大块输出在锁外限速，便于持久日志接收器持续收取；需要读到 snapshot end 才能认定这一份传输完整。容量有界，不等同于任意深度的间接命令、shader 和全部纹理内容快照。

原 GPU 组件入口已编出包含 26 个对象的 `libCoachzGpu.a`。修订只编译 GPU 驱动组件，未构建完整内核；既有 Metal 插件优化、addon 与配套信任缓存保持。需要后续既有内核组装流程带入该 GPU 修订后，再验收恢复、画面和首次挂起现场；尚未实机确认，不提交。

### 恢复修订的启动复验，尚未触发故障

后续启动使用上述冻结 GPU 库，以及同批 addon 和信任缓存。日志确认捕获已安装；最终提交与完成各 7452，shown 3559，帧失败、挂起、重启及恢复失败均为零，没有 DeviceLost 或目标进程崩溃记录。此次没有进入恢复或 snapshot，因此不能把它当作重放修复的实机故障验收。GPU 的 bank FSR 为 `0x400`，按主线 `arm-smmu.h` 的 FORMAT/FAULT 字段定义不含 fault bits；其他列出的硬件错误位为零。累计 28 次睡眠门控跳过核心寄存器访问，不将其省略为“全部访问正常”。

实际私有路径 run 1610，shader 参考稀疏审核 28/28/0，表示检查 verify 8、bad/missing/disabled 0。末次阈值采样的 writes/rotate 为 1024/1024、merged 0，1024 个发布边界全部只含一个原生 pass；合并规则虽已运行，本次负载没有提供合并机会，不能报告转换次数或帧率收益。该采样早于最终计数，不与最终总量强行对齐；字节计数仍为逻辑覆盖量。

帧 trace 前 311162 条记录可解释，之后出现零填充及无效记录，尾部原因未定。可信前缀中的 23699 个编码批次，排队均值约 7.98 ms、p95 34.59 ms，编码执行均值约 1.49 ms；这些是软件批次时间，不是 GPU 执行时间，也不能相加成每帧成本。5664 个命令缓冲的提交到回调均值约 27.92 ms，仍包含排队与软件开销。没有同操作窗口基线，不建立速度改善结论。GPU 源码和构建产物保持冻结；本轮只补充实机观察与验收边界。


### 导出、发布与 MRT 六项优化候选，待实机

本批包含六项独立源码改动；实机性能收益尚未验证。诊断、分析工具和重新构建不计为优化点，既有批次也不重复计数。

1. `coachz_export4` 的每个采样 invocation 写独立报告槽，移除集中在两个计数器上的全局原子加法，成功完成后在 CPU 汇总。采样位置、边缘谓词、像素写入和变化率估计保持。
2. 导出报告缓冲在成功 GPU 完成且 CPU 读取结束后进入有界 best-fit 复用池。失败、DeviceLost、验证报告和仍在使用的缓冲不进入空闲池，内存压力只回收空闲项。
3. 原生私有到规范私有的旋转发布使用同表面尚未发布的脏区域并集，并用保留原内容的局部传输更新。旧内容无效、无可靠写入记录、无边界写入或混合表示时保持整帧更新；可靠空区域不发出转换。版本和共享页发布完成边界保持。`TR native partial/empty/saved/rect` 记录命中与逻辑覆盖量，不能解释为 DRAM 流量。
4. 正常渲染的额外 MRT 保持原 `DontCare` load action，避免主动加入清零；诊断参考与变体仍使用一致的确定清零。Load 和 Store 原有语义保持。
5. 原生方向的多个 MRT seed 或 store 在同一个 blit encoder 中依次编码，各纹理继续保留到实际完成并记录自己的访问依赖。空集合不开 encoder，创建、复制或结束失败仍使命令失败；旋转路径保持原有传输。
6. 按源方向与比较策略选择四个专用导出内核，将旋转与比较参数固定在入口，供编译器消除统一参数读取、坐标选择与策略分支；专用管线编译失败则使用原通用内核。一次调度使用同一个比较策略快照。原 BGRA8 读取、打包、纹理创建及 UBWC/tiling 输入保持，`TR export specialized/failed/variant` 记录实际选择，不能以存在函数就断言已运行。

诊断沿用 Mesa 的 A6xx `u_trace` 时间戳机制，覆盖 compute、复制、清除、渲染和缓存刷新，shader 哈希与管线日志对应。Metal 提交、runtime 对象、Vulkan command buffer、实际 kernel fence 和呈现序号独立记录，再按身份关联。主机排队与 GPU 时间保持不同的时钟域，不把嵌套 GPU span 相加为独立成本。缺失时间戳和未完成批次保持可见。

帧记录按进程和 epoch 分文件，v3 每块校验元数据、载荷和结束标记，记录丢失量，处理短写、EINTR 和重试，并周期同步；第一次提交即启动写入器，不等待成功完成。采集需同轮保留 cache 内所有 `frames-*-*.trace`、`gpu-work-*-*.csv`、`pipelines-*-*.log`，使用 `tools/coachz_gpu_profile.py --frames FILE --work FILE --pipelines FILE --output FILE` 验证可信前缀、完整批次、PID/epoch 和实际 fence，缺失关联不补猜。探针对 GPU 和存储的开销未单独测得，固定窗口的性能比较须保持相同诊断配置。

本批需要同次生成的 addon 和配套信任缓存，继续携带 prime list 和两组壁纸；GPU 内核库保持当前恢复候选。使用原 GPU 构建入口和标准输出目录，不构建整颗内核。实机同轮检查六项命中、每种工作耗时、排队与 fence、画面方向/颜色/alpha、局部重绘、截图和切换快照，以及失败、DeviceLost、进程存活和恢复现场；未实机验收，不提交候选。

标准 `tools/xnu_gpu_userland.py --build` 入口现已完成本批编译与打包。addon 的 20 个文件已解包核对，三份实际代码签名与配套信任缓存一致，内嵌 AIR、内嵌与随包 prime list、两组壁纸均匹配；构建覆盖层已清理。GPU 内核库及其 107 个源码输入与当前恢复候选的哈希一致。这只是组件集成交付，六项的实机正确性与性能收益仍待同轮数据验收。

## k213 之后的八项源码修订（尚未实机验收）

本轮以 k213 的六项版本为基线，新增改动独立计数。探针、读取工具、删除单测和旧改动不计入八项。标准 GPU 构建入口在活动批次不足八项时，于应用补丁和启动编译之前拒绝构建；活动批次与构建结果保存在忽略目录。本轮不新增或运行单测，不修改其他子系统。

| 项目 | 新行为 | 必须确认的实机边界 |
|---|---|---|
| 1. 自有整屏绘制去掉额外清屏 | 不透明 scanout 和精确 RGBA 旋转传输使用 LOAD，避免运行时将 DontCare 改写成零清屏；局部更新保留范围外像素 | 首帧、两个 scanout 缓冲、旋转与颜色；实际 clear 区间是否消失 |
| 2. 导出只发一次 dispatch | 自带两轴边界检查的 packed-export 内核使用向上取整的完整工作组，消除精确 dispatchThreads 产生的独立尾部区域 | 右下边缘、不对齐矩形、实际 GPU dispatch 数量 |
| 3. 呈现读取已完成的原生方向图像 | 成功完成、写入 epoch 仍匹配、没有 plain writer 且几何/格式一致时，直接采样 native MRT 图像；继续保留 canonical 和共享页的发布 | 原生选择次数、GPU 时间与逐像素方向；旧/失败/未完成版本必须回退 |
| 4. 每个 scanout 缓冲分别维护 damage 历史 | 仅在来源与完整版本链都匹配时，对该缓冲按 damage scissor 绘制；线性采样向外扩边；不变版本可跳过绘制。来源变化、历史缺失或失败走整帧 | 双缓冲交替、跨多次源写入、历史淘汰、边界像素；不能将上一个显示缓冲的历史套给另一个 |
| 5. 命令内复用 MRT 临时图像 | 前一个 pass 的 render/store/seal 均记录成功后，同一命令中的后续 pass 可复用临时图像；保留 runtime 的资源依赖，命令之间仍只在成功完成后归还 | 多 pass 的读写排序、临时资源数量、颜色一致性；比较用 audit 图像不参与此复用 |
| 6. 兼容的私有辅助 MRT 直接绑定 | 原生方向下，对独立、非导入、非 view、非旋转、非 buffer、单层单级单采样的私有 RenderTarget，省掉 shadow 分配及前后的 LOAD/STORE 拷贝 | 实际命中、GMEM 选择、辅助附件内容；IOSurface/linear/audit 继续原路径 |
| 7. 辅助附件按实际区域拷回 | 对 shadow 路径的原生 LOAD/STORE 辅助附件，只拷回原始物理 scissor 并集；incoming seed 仍完整，保证 framebuffer fetch/helper 读取已初始化 | 范围外内容保持、局部矩形和实际字节数；Clear、audit、未知覆盖和非原生方向保留整帧 |
| 8. 每个目标独立学习导出策略 | 以 backing 的区域、布局和覆盖类别保存独立的 compare/store EWMA；只从成功完成的报告学习，保留原阈值与衰减，单 backing 最多 16 项元数据 | 不同静态/动画目标互不污染、淘汰时在途报告仍有效，以及同窗口实际 GPU 成本 |

这些是源码实现，不能代替实机性能结果。k213 对齐的十二秒窗口中，主合成命令的测得 GPU 区间合计均值为 33.598 ms，其中 2266×1488 双附件 pass 均值 28.89447 ms；present 自身 GPU 区间均值为 3.7588 ms，内含平均 1.4414 ms 的额外清屏，不能重复相加。现有资料不能把这 28.9 ms 归给某一个 shader；记录中的最大指令数并不表示该 shader 独占整个 pass。第八项等策略变化也不能仅凭源码宣称更快。

诊断源码同时补充了完整 present 阶段及空复制回执、native 写入版本回执、实际 scissor、Metal/runtime/Vulkan 命令与 fence 的关联、逐 draw 和多 draw 子绘制、间接批次参数、viewport/scissor 与附件写入/混合状态、实际 Vulkan 管线句柄的创建/销毁生命周期、原函数别名和选中可执行 shader 的完整哈希与统计。驱动与运行时回调采用显式注册和状态回执；不再用空 compute 哈希或 Vulkan 的 `main` 名称猜原函数。采集文件在初始化时预先打开并保留描述符，处理已观察到的延迟打开被 sandbox 拒绝问题。以上新增探针仍须随新组件上机确认，不能称为全进程采集已经通过。

读取工具按完成标记流式处理 Mesa CSV，按真实端点保留有效兄弟区间，把 0/0 条件分支单列为“未执行或未测得”，单端点缺失/倒退仍作为错误。完整回放 k213 的 backboardd 资料得到 58,312 个完成批次、2,650,194 个有效区间、676,064 个 0/0 分支，坏行和坏区间均为零，峰值待处理事件为 1,766。旧资料没有新回调元数据，因此不会补造 shader 名称。旧采集缺失 PID 287 的文件，不能据此声称覆盖所有进程。

## k213 帧时间的拆分与非阻塞翻转（待实机）

k213 播放窗口每帧 52.5 ms，顺序是：合成命令提交并交出帧，呈现器的缩放命令排在它后面，合成跑完再跑 3.8 ms 的缩放（交帧到呈现完成均值 36.5 ms），然后 `COMPLETE_SWAP` 同步等 CTL_FLUSH 在 vsync 清零（均值 9.0 ms），之后才归还渲染服务器的缓冲；再过约 5 ms 下一帧合成才开始。主线 `msm_atomic_commit_tail` 先 `flush_commit` 再 `wait_flush`，非阻塞提交的调用方不等 vsync，下一次提交前才等上一次完成。`coachz_scanout_flip` 照此只写 SSPP_SRC0 和 CTL_FLUSH 就返回，新的 `coachz_scanout_flip_wait` 在 `waitSwap` 交出下一帧之前等锁存（下一帧要画进锁存前还在扫描的那块缓冲），同一时间只有一个翻转在途。日志行 `COACHZ flip: … still pending at the next frame, wait mean …, request to seen latched mean …`。预期等待接近零、帧间隔约降到 42 ms；尚未实机。

更正：重合成 pass 的主目标在原生路径下已经压缩（Turnip `start_render_pass` 记 `ubwc=yy`，bin 256×256 共 54 个）；没走原生路径时主目标是渲染服务器的线性 IOSurface（`ubwc=ny`，bin 192×320 共 60 个）。第二个附件是渲染服务器的 DontCare/DontCare memoryless 输出，每像素多占 4 字节，bin 数因此约翻倍。带 STORAGE、`ubwc0` 的 IMG 记录多数是运行时比较内存需求时生成的临时布局。

逐 tile 的 `draw_ib_gmem` 区间按 Turnip 的蛇形 VSC pipe 顺序还原到屏幕坐标后，k213 里超过 20 ms 的重 pass（两种布局各约 300 次）成本集中在物理坐标约 x 0–1536、y 512–1280 的一块矩形：每个 bin 约 1.0 ms，其余界面约 0.3 ms，最右列约 0.1 ms。这块区域就是播放中的视频画面，约占整个 pass 的六成。全程统计中这种形态的 pass 中位数只有约 4 ms，播放时的 28.9 ms 来自视频图层的合成。着色器身份要等 k214 的管线关联；QuartzCore 中有 `CA_DISABLE_APL_FILTER`、`CA_DISABLE_TONEMAP_PRESCALE`、`CA_BACKDROP_SCALE`、`CA_DISABLE_FILTER_MERGING` 等开关，插件已在 backboardd 设置 CA 环境变量，确定着色器后再选。

同一个十二秒窗口（229 帧）按 GPU 工作分：重合成 pass 走原生路径 152 次、均值 27.8 ms，画进渲染服务器线性 IOSurface 77 次、均值 31.1 ms；整帧共享页导出 compute（`f27c013f`，35×186 组）152 次、均值 3.2 ms，与原生路径次数相同，每帧平均 2.2 ms；呈现缩放 229 次、3.75 ms；旋转转换 76 次、2.0 ms。全程计数 `turn try6051 run2823`，列出的拒绝原因（busy 186、wait 69）只占少数；呈现器 `miss tag1384` 次走共享页读取。下一批候选按收益排：视频图层的合成着色器（约 15 ms/帧，等 k214 的着色器身份）、只在有共享页读者时导出（约 2.2 ms/帧，需要跨进程和 CPU 读取的触发条件，不能简单省掉）、原生路径覆盖率（每次未覆盖约多 3.3 ms，需要 k214 按原因计数）、DPU VIG 缩放替代呈现 pass。

## 上游 UniXNU 11 月 7–11 日修订的移植与帧节奏（待实机）

上游 Mesa 仍钉在 `85fda3b`，与本地同一提交，下面五项以新补丁加到栈 overlay（`patches/mesa/coachz-port-*.patch`），按 `adreno-compiler-inputs.json` 的顺序排在既有三项之后，由 `tools/xnu_gpu_userland.py` 打上：

1. `ir3_shader.h`：片元阶段的 const 上限预留其他阶段在 safe 变体下的份额，避免 `ir3_trim_constlen` 为大片元着色器换上 `safe_constlen` 变体（上游 215672b：CoreAnimation 通用片元着色器在主机编译下 28587→22082 条指令、8369→4243 次停顿）。
2. `tu_knl_darwin.cc`：同一进程的所有 Turnip 设备共用一个 GPU 地址分配器和锁（上游 97437dc：第二个设备重用已占地址，内核拒绝映射）。
3. `tu_knl_darwin.cc`：内核故障计数变化（GPU 重启并重放其余提交）不再把 Vulkan 设备判为丢失，只计入 `tu_darwin_restarts`（上游 c72a8a5）。本地该处带有诊断补丁的报错文字，按同一语义手工移植。
4. `tu_clear_blit.cc`：线性附件按行距而非宽度判断能否用事件 blit 做 GMEM load/store（上游 6061e10）。
5. `tu_knl*.cc`、`tu_queue.cc`：超过内核 IB 上限的提交在命令缓冲开头处切开，不切开命令流（上游 d76f17d）。

插件在渲染服务器里按名字找到 `CA_FORCE_TRIPLE_BUFFERING`（21H461 的 `CAGetDebugOptionEnvVariableName` 只回答 0–180 号，`CASetDebugOption` 写字节并发出变更），设为 1；`ca-triple-buffer=0` 不设；结果以 `TB triple buffering option` 记入内核日志（上游 AdrenoMetal f8fbee7 的做法，选项名按本系统）。内核侧的卡死处理改为出手前比较 CP 的 IB 状态（上游 Adreno740 95789237），另记 `COACHZ gpu stall` 行。启动参数去掉 `sheng-early-retire=0`：旧版显示驱动在 GPU 呈现器取走帧时归还交换，帧内存由呈现器自己引用，下一帧的拷贝仍等这一帧完成。

不移植的：让 Turnip 逐 pass 自选渲染模式并开启并发分箱（9ddfaf2）。插件注释记录并发分箱曾使 BV 与 BR 在 pass 内互等，A618 没有这两条线程；渲染模式改动以前测得 SP 忙碌减半但提交到完成不降，另做对照。两平面 YCbCr 直接按平面采样（7841a9f）依赖新版翻译器 `555b13d5`，暂不移植。

## k215 原生路径被整进程关闭的原因

k215 的 backboardd（pid 55，被杀后重生为 pid 500）在原生路径启用后几秒内拒绝了 7 个命令缓冲：先是没有运行时错误文本的 `physical private seed:`，然后是 `deferred render encoder: … RenderPass(DefaultRasterSampleCount { count: 0 })`，即一个没有任何附件的 pass。拒绝落在含原生物理检查的命令里，检查读到 `valid0 rows0`，`gCoachzNativeDisabled` 置位后此进程所有候选都按 `prog` 拒绝、所有帧走线性 IOSurface 的 plain 路径，显示帧数约为 k213 的三分之一。八项里的“直接绑定私有辅助附件”把 `encoder->coachzAuditing=audit` 提到 `coachzPrepareMrt` 之前且无条件执行；一个已判为要审计、随后因限速、在途审计、空视图或队列不符被拒的候选仍带着审计标记。`coachzFlushDeferred` 照审计流程回放它：没准备过的视图让种子传输在 `coachzFrameTransfer` 的空指针检查处静默失败，`coachzSelectMrt` 把每个附件写成空句柄。日志里多出的一次原生种子（`seed3 reuse1`，打印出的选中 pass 只有一个）就是这样的回放；原生关掉后不再有审计，拒绝也正好停在 7 次。被拒的候选现在同时复位审计和选中标记，回放走原 pass。k215 里 scratch 复用、直接附件和局部回写三项一次也没有触发（没有 `TR scratch local`、`TR mrt direct`、`TR mrt partial`），它们的效果还没测到；backboardd 的内存增长是否只来自 plain 路径要看修复后的实机。

## k216 实机结果（上游移植八项加审计复位）

原生路径整次启动都开着：物理检查 8 次全部 `rows128`，没有命令缓冲被拒，backboardd 全程是同一进程（k215 里它被 jetsam 杀掉重启过）。`CZ turn try1569 run1096`，k213 是 `try6051 run2823`。翻转 1801 次，0 次超时，下一帧到来时仍未锁存的有 510 次，等待均值 2.1 ms、最大 17 ms（k213 同步等待均值 9.0 ms）。卡死处理没有出手（kicks 0，left alone 63）。

用同一个读取工具解析两次的 backboardd 采集，比较视频播放窗口内的 2266×1488 双附件 pass：k216 全部走原生路径，超过 20 ms 的重 pass 均值 22.6–23.8 ms；k213 原生 27.7 ms、线性 IOSurface 30.8 ms。1488×2266 旋转转换 pass 在 k216 里没有出现，呈现缩放 pass 均值 2.2 ms（k213 为 3.7–4.1 ms）。每帧 GPU 工作因此少了约三分之一，但重 pass 每秒只有 10–14 个（k213 约 19 个），这一时段 GPU 忙碌 52–62%，频率 800 MHz。

帧间隔由两段串联而成：呈现器等渲染服务器交帧（均值 21–40 ms），再等 GPU 跑完（呈现器 `gpu_wait` 35–43 ms，其中自己的缩放只占 2 ms，其余是排在它前面的合成）。呈现器处理上一帧期间，渲染服务器几乎没有提交（平均 0.04 个），要在上一帧完成之后约 12 ms（中位数）才提交下一帧。也就是说 GPU 上同一时间只有一帧，GPU 和编码 CPU 交替空闲。21H461 的 `CA_FORCE_TRIPLE_BUFFERING` 是 176 号调试选项（`CA_FORCE_DOUBLE_BUFFERING` 119 号，`CA_LOG_TRIPLE_BUFFERING` 13 号），`IOMFBDisplay::can_update_` 和 `finish_update` 每次更新都读它。插件的 `TB triple buffering option` 行没有进内核日志：它和同时发出的 `SC route` 行一样，在设备刚打开、通知通道还没建立时就发出了。所以这次无法确认选项是否已设上。`J completed wait`（n1930，均值 23.5 ms）与显示帧数一致，是呈现器自己的等待，不是 CA 在等。
