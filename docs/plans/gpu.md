# Adreno 618 GPU：路线和前三个阶段

`platform/coachz-prelink/patches/xnu/coachz-prelink-gpu.patch`（内核）和 `tools/xnu_screen.py` 的 `--gpu`、`--gmu-firmware`（镜像打包）已接入，前几级真机通过。

## 路线

整体路线按 `Trogdor-GPU-Research` 的结论走：苹果侧复用 UniXNU 的 AdrenoMetal、metal2vulkan 和 Turnip，内核侧给 SC7180 写 A618 后端。Coachz 是主线 Linux 正式支持的整机（`sc7180-trogdor-coachz-*`），所以顺序和寄存器值以主线的设备树和 `drm/msm` 为准，再用同一台机器上 Debian 读到的值核对。ios-extra 是 SM7150 手机上的记录，权限和这台不同，只作线索，它的进展和卡点都不算这里的预期。Linux 是 GPL，这里只取事实和顺序，代码自己写。每一阶段都必须能独立回滚，再进下一阶段。

| 阶段 | 内容 | 状态 |
|---|---|---|
| 1 | 时钟/电源清点；CX GDSC + 200 MHz GMU 时钟 + 总线分支窗口，读 GMU 寄存器后全部回滚 | 真机通过 |
| 2 | `a630_gmu.bin`（主线 Linux 给 A618 用的 legacy GMU 固件）写入 GMU ITCM/DTCM 并逐字读回；CM3 始终保持复位 | 真机通过（mode 3） |
| 3a | GPU SMMU（0x05040000）只读清点：全局配置、ID、故障、stream match group、各 context bank | 真机通过（mode 4） |
| 3b | GMU stream 5 的 context bank、页表、HFI 队列和日志缓冲区映射，用 SMMU 的地址转换操作核对 | 真机通过（mode 5 的内容，由 mode 6 那次运行验证） |
| 4a | RSCC/PDC 序列（0x0b290000 / 0x0b490000）、释放 CM3、等 `FW_INIT_RESULT` 签名，再把 CM3 放回复位 | 真机通过（mode 6），签名 `0xbabeface` |
| 4b | GX OOB、SPTPRAC，之后用 legacy HFI 启动（sweet 的两队列传输） | 真机通过（mode 7），固件版本 `0x10060000`，5 条消息全部成功 |
| 5 | 最低频率、stream 0 的 bank、`hw_init`、SQE 固件、ME_INIT，再让 CP 写内存（ring 和 IB 各一次）和 fence | 真机通过（mode 8）。跑完全部关掉，还不是常驻驱动 |
| 6 | 常驻驱动（mode 9）、Turnip Darwin 后端、Metal 发布、GPU 合成上屏 | 真机通过，界面由 GPU 合成，帧率还低 |

## 第一阶段做了什么

`coachz-gpu=1`：只读。
- GCC：GPLL0 mode/vote、GPU 分支投票寄存器 `0x52000`、cfg-ahb、memnoc、ddrss-axi、snoc-dvm。
- GPUCC：CX/GX GDSCR、CX 的 GDS 状态、GMU RCG、各 CBCR。
- 只有在 GPU AHB 分支运行时才读 GPUCC，否则访问会卡住总线。

`coachz-gpu=2`：在没有其他拥有者时：
1. GPU AHB 分支若停着，就打开它。
2. 清 CX GDSC 的 SW_COLLAPSE，按 Linux `gdsc.c` 的做法轮询 `0x1540` 的 PWR_ON，最多 2 ms。
3. 打开 GPLL0/2 门控（`0x52000` 的 bit16，与其他位共享，只改这一位）。
4. RCG `0x1120` 设为源 6、HID 2，即 200 MHz，等 UPDATE。
5. 打开 cxo 和 cx-gmu 分支，后者按 gpucc-sc7180 的推荐写入 wake/sleep 字段。
6. 打开 memnoc 和 ddrss-axi 投票分支。
7. 读九个 GMU 寄存器：CM3_SYSRESET、FW_BUSY、FW_INIT_RESULT、CM3_CFG、SPTPRAC、RPMh 电源状态、GENERAL_0、AO 中断、CX busy。
8. 按相反顺序撤销自己做过的每一处改动，再读一次全部寄存器。

刻意不做的：
- 不发 RPMh 投票：CX 是 SoC 主电源轨，系统运行时一直开着，这一点和需要单独投票的 SM7150 不同。
- 不碰 GX、固件、SMMU 或 GPU 核心寄存器。
- 不写 GMU。

任何前提不满足（GPLL0 未锁定、cx-gmu 已被别人打开）就什么都不改。任何一步超时就回滚。RCG 和 ios-extra 一样留在 200 MHz 配置，它下面没有打开的分支。

SC7180 与 SM7150 不同的地方都已按 Linux 源码改过，例如 GPU 的 GPLL0 门控在 `0x52000`，而 SM7150 在 `0x52004`。`tests/test_gpu.py` 逐项检查这些值。GMU 偏移按 `a6xx_gmu.xml` 的 dword 偏移 × 4 − 0x6a000 计算，例如 CM3_SYSRESET 是 0x14000，与 ios-extra 在 A618 上实测读到的位置一致。

## 第二阶段做了什么

`coachz-gpu=3`：先进入第一阶段的 CX + GMU 时钟窗口，在九个 GMU 寄存器读完之后、回滚之前：

1. CM3_SYSRESET 的 bit0 必须为 1，也就是 CM3 处于复位，否则不写，记一条错误。
2. 把固件按 32 位字逐个写到 GMU 窗口 +0x3000 起的 TCM。前 16 KiB 是 ITCM，后 16 KiB 是 DTCM（+0x7000）。这和主线 `a6xx_gmu_fw_load` 对 legacy 固件的做法一样：整个镜像从 ITCM_START 开始写，最多 32 KiB。
3. 全部读回比对，记下不一致的个数和第一个位置，分别算源镜像和读回内容的 FNV-1a。
4. 再读一次 CM3_SYSRESET，确认 CM3 仍在复位。
5. 照常回滚。CX 一关，TCM 内容就没了，所以每次启动都是从头写。

不写 GENERAL_7（TCM 保持），也不做 RSCC/PDC，不释放 CM3。这些属于第四阶段。

**固件怎么进来**：
- 固件不进仓库。
- `xnu_screen.py --gpu 3 --gmu-firmware FILE` 把文件接在信任缓存后面，按 4 KiB 对齐，放在加载器的内核数据区里（`boot/xnu_screen.c` 的 top 覆盖 ramdisk_at 之后的全部内容）。
- 同时在 `/chosen/memory-map` 写入 `CoachzGmuFirmware`（物理基址、大小）。
- 内核用 `SecureDTGetProperty` 取出这个条目，再用 `ml_static_ptovirt` 读，和 RAM 盘、信任缓存是同一套做法。

**固件来源**：linux-firmware 的 `qcom/a630_gmu.bin`，32768 字节，正好填满 ITCM+DTCM。取用时 linux-firmware 的 commit 是 `bda6eccc3a8f52e00696cf733b6e4f042d56655c`，sha256 是 `da8d9b1b1f5c1a0b311f32567093b4828f3c80031dd8435f91ac13c664e173a6`。Debian 的 `/lib/firmware/qcom/a630_gmu.bin` 如果是同一个文件，也可以直接用，用之前先核对 sha256。ios-extra 在 A618 上写入过同一份文件，8192 个字读回零差错。

## 第三阶段（3a）做了什么

`coachz-gpu=4`：先进入第一阶段的 CX + GMU 时钟窗口，读完 GMU 寄存器后，**只读** GPU SMMU，然后照常回滚。不写任何 SMMU 寄存器。

为什么在这个窗口里读：
- sc7180.dtsi 里 adreno_smmu 的电源域是 CX GDSC，时钟是 memnoc（bus）和 cfg-ahb（iface）。
- 窗口里这三样都开着，窗口外 SMMU 寄存器不可达。

读哪些（ARM SMMUv2 架构的公开寄存器布局）：
- 全局空间 0：sCR0、sCR1、sCR2、sACR、IDR0、IDR1、IDR2、IDR7、sGFSR、sGFSYNR0。
- 按 IDR0 的 NUMSMRG：每个 SMR 和 S2CR，最多 16 个。
- 全局空间 1（起点是一个 SMMU 页之后）：每个 CBAR 和 CBA2R。
- 按 IDR1 的 NUMCB：每个 context bank 的 SCTLR、TCR、FSR，最多 8 个。

**边界**：SMMU 页大小、全局空间页数和 context bank 起点都从 IDR1 算。算出来的布局超出 64 KiB 窗口（或 ID 寄存器读出全 1）时，读完全局寄存器就停下，记一条错误。

**目的**：在设计 3b 之前先知道真机上的 SMMU 现在是什么状态，包括是否已启用、各 stream 走哪个 context bank、有没有空闲 bank、固件是否留下了配置。3b 只在这些读数的基础上，按 Linux arm-smmu 的标准流程编程。

## 第 3b、4a 阶段（mode 5、6）做了什么

代码在 `bsd/dev/coachz_gpu_gmu.h`（补丁内），都在第一阶段的 CX + GMU 时钟窗口里运行。

`coachz-gpu=5`：
- 内存放在低端 DRAM 的 `0x81400000`（64 KiB，内核窗口之外，映射为不缓存）。SMMU 的表遍历是非一致性的，这样不用做缓存维护。
- IOVA 按 Linux 的 legacy 布局：dummy `0x60000000`（4 KiB），debug、日志、HFI 各 16 KiB，紧跟其后。
- bank 1、group 1 给 stream 5，bank 0、group 0 留给 GPU。4 KiB 页，32 位 VA，三级页表。未匹配的 stream 继续旁路。
- 用 SMMU 自带的 ATS1PR 核对：每个区域的首页和末页要翻到正确的物理地址，3 个未映射地址要返回故障。这一步不需要 GMU 产生流量。
- 只要 SMMU 不是空闲状态（sCR0 已启用、有有效的 SMR、bank 已开）就什么都不写。

`coachz-gpu=6`：只有 11 个转换全部正确才继续。写 RSC 和 PDC 的序列与 TCS 命令，唤醒 RSC，写入固件，配置 HFI 地址、日志地址、芯片 ID 和投票使能，放开 CM3，等 `FW_INIT_RESULT`。结果读完后把 CM3 放回复位，停 RSC，恢复 SMMU，关 CX。PDC 的内容留着，Linux 也不清，GPU 关着时没有东西启动它。

`coachz-gpu=7`：拿到签名之后继续，顺序照 `a6xx_gmu_resume`：
- 从 CMD DB（`0x80820000`，只读）取 `gfx.lvl`、`mx.lvl`、`cx.lvl` 三张等级表，算出每一档的投票。GPU 的档位是这块板子的 OPP 表：180 到 800 MHz，再加 Linux 补的 “off” 一档；825 MHz 要更高的 speed bin。GMU 的档位是 200 MHz。
- 投最高档，发 `BOOT_SLUMBER` OOB，固件给 GX 上电并确认。
- 打开 SPTPRAC，写 `HFI_CTRL_INIT`，发 `GMU_INIT`、`FW_VERSION`、`PERF_TABLE`、`BW_TABLE`、`TEST`。A618 的 GMU 不做总线调频，`BW_TABLE` 是一档 “off” 加固定的 CNOC 投票。
- 发 `GPU_SET` OOB 并确认，再清掉。GPU 核心寄存器一个都不碰。
- 结束时走 `a6xx_gmu_force_off` 去掉 GPU 核心部分的流程：SPTPRAC 关，等 4 个 TCS 空闲，清 fence，CM3 复位，RSC 停，GX 折叠。

`coachz-gpu=8`（`bsd/dev/coachz_gpu_core.h`，`--sqe-firmware` 传入 `a630_sqe.fw`，不进仓库）：
- `GPU_SET` 之前用 DCVS 把频率设到最低档 180 MHz。
- 同一个池里给 stream 0 建 bank 0，全局地址从 16 MiB 起，CPU 侧不缓存。这次把 `USFCFG` 打开，未匹配的 stream 会报错，不会旁路进 DRAM。
- `hw_init` 按 A618 的分支写寄存器；时钟门控 63 项和 CP 保护 32 项，都用脚本从 Linux 的 `a615_hwcg`、`a630_protect_regs` 转换。
- SQE 固件去掉第一个字，写进池里读回核对，版本不低于 1.90 才继续。
- 启动 SQE，发 ME_INIT，再发一次 `CP_MEM_WRITE`、一个带 `CP_MEM_WRITE` 的 IB 和一个 `CACHE_FLUSH_TS`。每次都用 `WHERE_AM_I` 让 CP 报告读指针。
- 结束时停 SQE，GBIF 停止并软复位，再走 mode 7 的关机流程。`GPU_SET` 一直保持到这之后，避免 GX 在读寄存器时掉电。

GPU 核心寄存器不能用 64 位写。很多寄存器不是 8 字节对齐，Device 内存上的非对齐访问会 abort。k67 就是这样 panic 的，所以和 Linux 一样拆成两次 32 位。

每个可能卡住机器的步骤之前都打印一行 `step N next` 并等 1.5 秒，让日志环落盘。

## 第 6 阶段（mode 9）：常驻驱动和 GPU 合成

机主决定把常驻内核驱动、Turnip、Metal 和合成器合并成一张镜像一起测。`coachz-gpu=9` 走完 mode 8 的 `hw_init` 就不再回滚，GPU_SET 一直保持，没有电源折叠，也没有中断（等待方轮询）。

内核这边：
- `platform/coachz-prelink/package/CoachzGpu.cpp` 是板级代码。Adreno740 的框架（内存、地址空间、提交、IOUserClient）沿用，补丁在 `patches/gpu/Adreno740.patch`：内核对象移到 16 MiB 以上（A618 的 GMEM 在 1 MiB），ring 改成 `a6xx_submit` 的帧，关掉硬件读指针影子。
- SMMU 用 UniXNU 的 ArmSmmu4K。它要求全局配置先开 GFIE 和 USFCFG，空闲的 stream 组先设成 fault，所以这两步放在它之前。bank 0 给 GPU，bank 1 给 GMU。
- `AGXAccelerator` 派生自原版 `IOAcceleratorES`，基类地址按审计记录绑定，运行时再核对一次。
- launchd 最多等驱动 20 秒，然后写插件并挂载（`coachz-prelink-gpu-addon.patch`）。
- 显示经 `sheng-fb-gpu=1` 交接。GPU 客户端没连上时，继续用 CPU 拷贝，所以驱动失败也不会没有画面。
- ADT 的 `artwork-display-gamut` 必须是 sRGB。P3 时合成器选 `b3a8` 表面，显示驱动不接受。

上层：Turnip 和 Metal 插件按 UniXNU 的脚本编译（`build-turnip-darwin.py`、`build-adreno-metal-plugin.py`），`tools/xnu_gpu_userland.py` 打成 addon 和信任缓存。打包参数是 `--gpu 9 --gmu-firmware ... --sqe-firmware ... --gpu-addon ... --root-trust-cache ...`。工具需要 glslang、ninja、shaderc、llvm、vulkan-headers、Rust 的 `aarch64-apple-ios` 目标，以及 `.cache` 下的 meson、mako、bison 3.8.2，版本钉在 UniXNU 的 `scripts/data` 里。插件的头文件还用旧目录名，要在 `drivers/` 下把 `NativeMetal`、`NativeMobileDisplay` 链接到 `AdrenoMetal`、`GopFramebuffer`。主机仿真链接时缺 `proc_selfpid`，要临时补桩；改完要还原，否则 `kernel_source.py` 的干净检查不通过。

内存：XNU 只接受一段连续内存。这台 4 GiB 地址以上有连续 6 GiB，所以 `--memory 0x180000000`。默认的 1 GiB 放不下 BO 池，768 MiB 的预留曾让 jetsam 杀光进程。池现在 512 MiB（`coachz-gpu-pool`），从内核内存里要。

每 10 秒的 `COACHZ gpu9:` 行报告插件连接、ring 提交和帧数。

## 主机验证

- `python3 -m unittest tests.test_gpu`：5 项通过。
  - `tests/coachz_gpu_model_test.c` 在一个寄存器模型上跑 16 个场景，在 ASan/UBSan 下同样通过：
    - 第一阶段：只读、AHB 关、完整窗口、AHB 自管、CX 超时、cx-gmu 已被占用、GPLL0 未锁。
    - 第二阶段：32 KiB 写入并读回、CM3 不在复位时拒绝、没有固件或固件超长时完全不动硬件、某个 TCM 字卡住时报出正确位置。
    - 第三阶段：完整的 SMMU 清点（读次数精确）、ID 寄存器描述的布局超出窗口时只读全局寄存器、mode 2 完全不读 SMMU。
  - 模型只在 CX 上电、AHB 和 memnoc 开着时允许读 SMMU，任何 SMMU 写入都记为故障。
  - 模型只允许 mode 3 在 CM3 复位、CX 上电、GMU 有时钟时写 TCM 范围，其他 GMU 写入都记为故障。
- `python3 -m unittest tests.test_gpu`：另有一项检查 SMMU 只在 mode 4、总线分支打开之后、回滚之前读取，源码里没有任何 SMMU 写。
  - 模型把以下行为都记为故障：AHB 停时访问 GPUCC、CX 或 GMU 时钟没开时读 GMU、写 GMU 或 GX、改动共享寄存器的其他位。
  - 每个场景都要求零故障，且撤销后状态与开始时一致（RCG 和 wake/sleep 字段除外）。
- 补丁能 dry-run 打到当前内核树上：它以已在 `series` 里的扬声器启动行作为上下文。
- 把 `xnu_screen.patch` 打到一份 `tools/xnu_screen.py` 副本上，检查 `CoachzGmuFirmware` 紧接在按 4 KiB 补齐的信任缓存之后，并且只有 `--gpu 3` 时接受固件。
- `coachz_gpu.c` 用 `coachz_sample.c` 的内核编译参数加 `-Werror` 编译无警告，未定义符号只有内核导出的函数。

## 接入

内核补丁已在 `series` 里。没有 `--gpu` 时内核不做任何 GPU 操作。打包时依次用 `--gpu 1`、`--gpu 2`、`--gpu 3 --gmu-firmware 路径/a630_gmu.bin`、`--gpu 4`。真机结果在 `docs/hardware-tests.md`。

## 真机上要看什么

日志进外接盘末尾的日志环，用 `sudo -n /usr/local/sbin/coachz-read-log` 读，找 `COACHZ gpu:` 开头的行。

- `=1`：`mode 1 stage 1 restored 0: ok`，加 `gcc before`、`gpucc before` 两行。期望值：
  - GPLL0 mode 的 bit31 为 1。
  - cfg-ahb 的 bit31 为 0（正在运行）。
  - CX GDSCR 为 `0x00222001` 一类值，即 SW_COLLAPSE 置位、未上电。
  - cx-gmu 的 bit0 为 0。
- `=2`：`mode 2 stage 7 restored 1: ok`。
  - `gpucc on` 里 CX 状态的 bit31 为 1，cx-gmu 的 bit31 为 0。
  - `gmu` 一行的第一个值 CM3_SYSRESET 应为 1（CM3 处于复位）。
  - `after` 两行与 `before` 一致，RCG cfg 和 cx-gmu 的 wake/sleep 字段除外。
  - 系统照常进主屏，显示、触摸和外接盘都不受影响。
- `=3`：`mode 3 stage 7 restored 1: ok`，另有一行：
  ```
  firmware 32768 bytes fnv 0x…, TCM 8192 words read back fnv 0x…（与前一个相同）, 0 mismatches (first 0), cm3 reset after 0x1
  ```
  其余各行与 `=2` 相同。
- `=4`：`mode 4 stage 7 restored 1: ok`，另有以下几行：
  - `smmu` 一行：10 个全局寄存器。
  - `smmu N groups M banks, page 0x…, banks at +0x…`。
  - `smmu smr`、`smmu s2cr`、`smmu cbar`、`smmu cba2r` 各一行。
  - 每个 context bank 一行 `smmu cbN SCTLR TCR FSR`。

  这些读数原样保留，3b 据此设计。
- `=6`：`mode 6 stage 7 restored 1: ok`，另有以下几行：
  - `smmu map step 4: 11 of 11 translations right`，`sCR0 on 0x1812 after 0x1`，`fsr 0x400`。这个 fsr 只是 FORMAT 字段（AArch64），没有故障位。
  - `init result 0xbabeface after N polls`。
  - `cm3 reset after 0x1`。
- `=7`：`mode 7 stage 7 restored 1: ok`，另有以下几行：
  - `boot oob polls N`、`gpu oob polls N` 都不为 0，`hfi 5 messages done ... status 0x0`。
  - `gx state`：`BOOT_SLUMBER` 之后 INTR_INFO 的 bit30 为 1，GX GDSCR 的 bit31 为 1（已上电），SPTPRAC 状态 `0x28`；`GPU_SET` 之后 INTR_INFO 的 bit24 为 1。
  - `shutdown sptprac off polls N, tcs idle 4 of 4`。`gpucc after` 里 GX GDSCR 回到启动前的值。
- `=8`：`mode 8 stage 7 restored 1: ok`，另有以下几行：
  - `cp results: ring write polls N, indirect buffer write polls N, fence polls N, read pointer shadow N of N`，轮询都不为 0，两个 N 相等。
  - `cp state`：`RBBM_STATUS` 为 `1`（只有 CPU 自己在读），HW fault、CP 中断和 protect 都是 0，bank 0 的 fsr 只有 `0x400`。
- 卡死或异常重启时，说明某个前提不成立，先退回上一个阶段看结果。

## 性能（k73、k74）

时钟、忙碌率和帧分解的原始数字记在 `docs/hardware-tests.md`。结论：

- GPU 实际跑在 650 MHz，计数器每秒都一致，最高忙碌 56%。频率和 GPU 本身都不是瓶颈。
- 每个 pass 都改成系统内存（k74 用插件补丁，k75 起用启动参数 `sheng-tu-debug=sysmem,nocb`）后，GPU 段快了约 13%，但重帧间隔没变。A618 的 GMEM 只有 512 KiB，这个分辨率要分约 30 个 bin。
- 重帧间隔约等于 CPU 编码加 GPU 执行：渲染服务器每帧要同步等完成，命令缓冲翻译和提交还另要 5～8 ms。UniXNU 在当前锁定版本之后，给渲染服务器加了独立的编码线程，提高了队列优先级，也改了导入和等待。下一步是同步这部分上游，再按同样的计数器对比。
- k75 起，上层（Turnip、运行时、插件）从 `upstream/unixnu-gpu` 构建，`tools/xnu_gpu_userland.py --build`。内核框架不变，只补了新上层的两个方法。上游这个版本有两处在 iOS 17 上编不过，构建时临时绕开，构建完撤回：主机仿真改用固定版本树里编好的那份（新框架的源码在主机上编不过），插件里只给 iOS 27 用的一个函数加 `[[maybe_unused]]`（`patches/gpu/unixnu-gpu-plugin.patch`）。
- k78 的硬件计数器（`--diagnostics` 下每秒一行 `COACHZ gpu prof`）：GPU 忙时 SP 大多在等 RB 和 UCHE，ALU 只占 4～15%，UCHE 延迟 180～370 周期，总线不到 2 GB/s。LLCC 在 XNU 下完全没配置。下一步照主线 `llcc-qcom.c` 配置 sc7180 的切片，再照 `a6xx_llc_activate` 激活 GPU 和 GPUHTW 切片。
- k79 照主线配置 LLCC 后，提交到完成快了约 25%。k80 的前台优先排队在 ring 前生效，但这台机器上和渲染服务器抢 GPU 的主要是它自己每帧之后的那次复制，别的进程很少。
- 上游的 GPU 调度是按前后台优先级分到两条 ring，靠 CP 抢占切换。主线 Linux 不给 A6xx 开抢占，只有一条 ring，所以 A618 只能在这一条 ring 前按优先级排队，不能打断正在跑的作业。

## 中断

SPI 300（`RBBM_INT_0`）、304（HFI）、305（GMU AO）已接（`CoachzGpu.cpp` 的 `gpuInterruptsOn`）。真机上 2312 次中断对应 2312 个 fence，错误位全 0；等 fence 的线程睡在 `CACHE_FLUSH_TS` 上，最多 1 ms 后重查，中断打开之前退回 `IODelay`。

## 电源管理

主线 a618 没有 IFPC，所以照 66 ms 自动挂起和 `a6xx_gmu_shutdown` 的顺序：停核心、清 GPU_SET、等空闲、SPTP 断电、BOOT_SLUMBER、等 CX 忙位、停 HFI、复位 CM3、停 RSC、断 GX。恢复走完整冷启动，重启环并发 ME_INIT。GX 断电后碰核心寄存器会卡死总线，所以 `coreRead`/`coreWrite` 都过 `pmEnter` 这道闸门；提交前的 `pmGet` 负责唤醒。真机上 slumber 约 1 ms、恢复约 11 ms，状态全 0，按电源键熄屏再点亮后界面正常。系统睡眠还没接。

## 还没做的

表里的阶段都走通了，接下来是性能。另外：
- 迁移到上游按进程 VM 的 Adreno740 框架（去掉 4 个窗口的限制，带挂起恢复）。
- 带宽投票（interconnect / BCM）还没有 RPMh 客户端。GX 的 ARC 投票不需要：表从 CMD DB 读，由 GMU 固件代投。
