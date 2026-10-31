# Adreno 618 GPU：路线和前三个阶段

`platform/coachz-prelink/patches/xnu/coachz-prelink-gpu.patch`（内核）和 `tools/xnu_screen.py` 的 `--gpu`、`--gmu-firmware`（镜像打包）已接入，前几级真机通过。

## 路线

整体路线按 `Trogdor-GPU-Research` 的结论走：苹果侧复用 UniXNU 的 AdrenoMetal、metal2vulkan 和 Turnip，内核侧给 SC7180 写 A618 后端。真机顺序参考 ios-extra 在 A618（SM7150）上逐步测过的 0083–0100，寄存器值逐个按 SC7180 重新核对。每一阶段都必须能独立回滚，再进下一阶段。

| 阶段 | 内容 | 状态 |
|---|---|---|
| 1 | 时钟/电源清点；CX GDSC + 200 MHz GMU 时钟 + 总线分支窗口，读 GMU 寄存器后全部回滚 | 真机通过 |
| 2 | `a630_gmu.bin`（主线 Linux 给 A618 用的 legacy GMU 固件）写入 GMU ITCM/DTCM 并逐字读回；CM3 始终保持复位 | 真机通过（mode 3） |
| 3a | GPU SMMU（0x05040000）只读清点：全局配置、ID、故障、stream match group、各 context bank | 真机通过（mode 4） |
| 3b | GMU stream 5 的 context bank、页表、HFI 队列和日志缓冲区映射 | 待写。按 3a 读到的真机状态设计，参考 Linux arm-smmu 和 UniXNU ArmSmmu4K |
| 4 | RSCC/PDC 序列（0x0b290000 / 0x0b490000）、释放 CM3、等 `FW_INIT_RESULT` 签名；之后用 legacy HFI 启动（sweet 的两队列传输） | 待写。ios-extra 在这里拿到了 `0xbabeface` 签名，随后卡在 GX OOB ACK |
| 5 | GX 上电、SQE 固件、A6xx CP 初始化、ring 写内存和 fence | 待写。CP 初始化格式和 A740 不同 |
| 6 | Turnip Darwin KMD、Metal 发布、接系统合成器 | 待写 |

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
- 卡死或异常重启时，说明某个前提不成立，先退回上一个阶段看结果。

## 还没做的

见上表第 3b–6 阶段。另外：
- GPU 中断（SPI 300/304/305）还没接。
- 带宽投票（interconnect / BCM）和 GX 的 ARC 投票还没有 RPMh 客户端，这是第 4/5 阶段的前置。
