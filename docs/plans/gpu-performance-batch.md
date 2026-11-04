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
