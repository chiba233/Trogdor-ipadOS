# Trogdor-ipadOS

HP Chromebook x2 11（Google Coachz / SC7180，骁龙 7c）的 **原生 iPadOS 移植实验**。不是虚拟机：原厂固件链启动，XNU 直接跑在这颗芯片上。

从内置 eMMC 启动 iPadOS 17.7.11，界面由 Adreno 618 经 Metal 合成上屏，触摸、键盘、声音、电池和背光可用，八个 CPU 核都在工作。每一部分的状态和证据见[当前进度](#当前进度)。这是一个实验项目，离日常使用还很远。

## 架构

### 启动链

原厂固件一个字节都不改，靠开发者模式的签名 FIT 进入自己的代码。

```mermaid
flowchart TD
    A["原厂 Coreboot、TF-A BL31"] --> B["Depthcharge（NS EL2）"]
    B --> C["开发者模式签名 FIT<br/>来自 eMMC 的内核分区"]
    C --> D["ARM64 探针：切到 EL1，恢复屏幕和背光<br/>装载内核时依次显示 R、G、B 启动图"]
    D --> E["XNU + iPadOS 的 Apple 原始组件<br/>prelink 成一个内核"]
    E --> F["eMMC：OS 卷和 SystemOS 只读<br/>Data 卷可写，APFS"]
    F --> G["Apple 原版 launchd"]
    G --> H["backboardd、SpringBoard、App"]
```

### 系统分层

上面是 Apple 的原版程序和组件，下面的驱动把它们接到这台机器的硬件上。硬件知识来自主线 Linux，iPadOS 适配的做法来自 UniXNU。

```mermaid
flowchart TB
    subgraph APPLE["Apple 原版（IPSW 提供，不进仓库）"]
        direction LR
        A1["launchd、backboardd<br/>SpringBoard、App"]
        A2["CoreAudio、QuartzCore<br/>Metal 框架"]
        A3["内核组件：APFS、AMFI<br/>IOHIDFamily、IOGPUFamily 等"]
    end
    subgraph OURS["这个项目和 UniXNU 的驱动"]
        direction LR
        D1["显示<br/>GopFramebuffer"]
        D2["输入<br/>触摸、USB 键盘、音量键、电源键"]
        D3["存储<br/>USB xHCI、大容量存储、eMMC"]
        D4["音频<br/>LPASS、HAL 插件"]
        D5["电源<br/>Chrome EC：电池、背光"]
        D6["GPU<br/>见下图"]
    end
    KERNEL["XNU（Darwin 23 源码，修补后编译）<br/>8 核 SMP，GICv3，定时器"]
    subgraph HW["SC7180 硬件"]
        direction LR
        H1["MDSS 面板"]
        H2["GENI I2C、GT7375P"]
        H3["DWC3 USB"]
        H4["LPASS MI2S"]
        H5["Chrome EC"]
        H6["Adreno 618"]
    end
    APPLE --> OURS --> KERNEL --> HW
```

### GPU

这是目前的重点。路线很明确：**硬件这边照主线 Linux，iPadOS 这边用 UniXNU 现成的 Metal 翻译层**，中间的 A618 内核后端是这个项目改编的。Turnip 只依赖一组固定的内核接口，所以上层基本不用动。

图里每一层都在真机上跑通了。和性能有关的做法：IO 一致性（导入内存不在每次提交前后清理缓存）、DDR 和内存总线的带宽投票、LLCC 系统缓存、系统内存渲染（tile 内存不更快，还会红蓝互换）、着色器预热、按负载调频。

```mermaid
flowchart TB
    APP["QuartzCore（CARenderServer）"]:::ok
    subgraph US["系统进程里"]
        META["AdrenoMetal 插件<br/>发布 Metal 设备"]:::ok
        M2V["metal2vulkan<br/>AIR → SPIR-V"]:::ok
        TU["Turnip（Mesa）<br/>SPIR-V → NIR → IR3"]:::ok
    end
    subgraph KS["内核里"]
        KMD["A618 内核驱动<br/>内存、地址空间、提交、fence"]:::ok
        GMU["GMU：固件、HFI、电源"]:::ok
        SMMU["GPU SMMU：页表"]:::ok
        CP["CP：SQE、ring、IB、fence"]:::ok
    end
    GPU["Adreno 618"]:::ok
    SCAN["合成结果送到 scanout"]:::ok
    SCREEN["屏幕"]
    APP --> META --> M2V --> TU --> KMD
    KMD --> GMU --> GPU
    KMD --> SMMU --> GPU
    KMD --> CP --> GPU
    GPU --> SCAN --> SCREEN
    classDef ok fill:#2e7d32,color:#ffffff,stroke:#1b5e20
```

GPU 的启动顺序，寄存器和顺序都按主线 Linux：

```mermaid
flowchart LR
    S1["CX 上电<br/>GMU 时钟"] --> S2["GMU 固件<br/>写入 TCM"]
    S2 --> S3["SMMU 页表<br/>11 个地址核对"]
    S3 --> S4["RSC、PDC<br/>放开 CM3"]
    S4 --> S5["固件签名<br/>0xbabeface"]
    S5 --> S6["CMD DB 投票<br/>GX 上电"]
    S6 --> S7["HFI 5 条消息<br/>GPU_SET"]
    S7 --> S8["hw_init<br/>SQE 启动"]
    S8 --> S9["ME_INIT<br/>CP 写内存和 fence"]
```

之后 GPU 保持开机，板级代码把它交给 Turnip 和 Metal。详情见 [GPU 计划](docs/plans/gpu.md) 和 [硬件记录](docs/hardware-tests.md)。

## 当前进度

| 阶段 | 状态 | 证据 |
| --- | --- | --- |
| 原厂启动链、XNU 早期检查点、内核初始化、prelink 和原版 AMFI | 通过 | 真机日志逐字段校验 |
| 启动菜单 | 通过 | 音量键或键盘上下键选择，电源键或回车确认，iPadOS 和 Debian 都能启动，从菜单进的 Debian 有网络 |
| 从 eMMC（GPT、APFS）启动主系统，进入主屏幕和 Dock | 通过 | 拔掉外接盘后屏幕和日志确认，启动菜单仍可选 Debian；未封存的根卷是放行，不是验证 |
| eMMC 上的 Data 卷可写 | 通过 | 从外接盘逐字节搬来后校验一致，保存的 Wi-Fi 密码还在，也能换壁纸；跨重启保留：上一次开机写下的标记下一次读到，LaunchServices 不再重建 |
| eMMC、microSD | 部分 | 照主线 `sdhci-msm` 移植，和 Debian 一样走 HS400 增强选通（200 MHz，8 位）；写操作只开放给切出的 scratch、日志和 Data 三个分区，窗口外的写被拒绝，2048 块写后读回无错，重启后内容还在；路径带宽已投票；刚写过的数据读约 113 MB/s、写约 61 MB/s，连续读没写过的区域约 60 MB/s（Debian 约 152 MB/s），差距还没查清；GPT 分区都已发布；microSD 卡读写真机通过：每次开机自检写 2048 块后读回无错，窗口外的 5 次写都被拒绝，写约 4.5 MB/s、读约 14–19 MB/s，比 Debian 的 SDR104 慢，还没提速；卡发布为块设备，系统里没有挂载卷 |
| 触摸 | 通过 | 照主线 `i2c-hid`、Goodix 和 `hid-multitouch` 移植，含 `input_mt_get_slot_by_key` 的槽位分配；实测大面积手指乱搓后单指仍正常 |
| 显示 | 部分 | 系统按 iPad mini 6 的 2266x1488 排版（比例 1.523，面板 1.5），GPU 每帧缩放到 2160x1440 面板，主屏布局对了；帧画进屏幕外的缓冲，等 GPU fence 完成后在硬件 vsync 翻转（照主线写 SSPP 地址和 CTL_FLUSH），日志里 1200 多次翻转 0 超时，平均等 7.5 ms；看不出撕裂了；系统收到的 vsync 事件还是 60 Hz 软件定时，没有直接扫描输出；panic 时切回固件缓冲的代码还没被实际走过；桥接芯片改为 24 bpp 后白色场景的色阶消失；界面方向正确并能转屏；开了真实小组件和壁纸后明显变卡，打开“降低透明度”和“减弱动态效果”也没有改善，换成 iPad mini 身份后非常卡，那次开机后约 70 秒 8 个核都接近满载，还因 GPU 设备丢失重启过一次界面，原因还在查 |
| App | 部分 | Safari 能打开 HTTPS 网页；设置正常，日期和时间页、关于本机都能打开；可删除的系统 App 从 OS 卷挂到 `/Applications`，主屏显示真实图标；壁纸设置页能打开，手动选的壁纸能显示，默认壁纸还是黑的；时钟、备忘录、新闻小组件有内容，部分小组件还是白块，小组件渲染进程会因 GPU 访问被撤销而崩溃重启 |
| 电池、屏幕背光 | 通过 | 状态栏电量和 Debian 一致，亮度滑块可用；经 EC 的 I2C 隧道读电量计，循环次数 68 和温度约 31 °C 都和 Debian 一致；预计时间和充电器信息发布给系统，拔插充电器正常；低电量警告已发布，但没有真正放电到那个电量测过 |
| 音量键、电源键 | 通过 | 音量提示正常且流畅；电源键熄屏后 GPU 断电，再按能恢复界面；系统休眠还没做 |
| Zed 键盘和触控板 | 部分 | 打字、顶排亮度和音量键、触控板的指针和滚动方向都正常，其他功能键未逐个确认；开机后拆下、装回都能认到，根盘不受影响；系统卡时指针也跟着卡 |
| 系统音频 | 通过（听感） | 经原版音频服务播放，听起来正常，日志无校正、无崩溃；路由显示为 iPad，图标是音响 |
| 麦克风 | 通过（听感） | 照主线走 ADAU7002、Primary MI2S 和 WRDMA，自检读到两路真实信号，游标速率对得上 48 kHz；两路去直流后混合并加 12 dB；语音备忘录录音回放正常，录音无直流偏置（均值 −2），3.5 秒内只有 2 段短零段，最长 17 帧；只录过几秒，长时间录音和系统休眠没测 |
| USB-C | 部分 | 内核自己初始化 DWC3 和 QUSB2/QMP PHY，键盘和触控板经它枚举正常；两个口的连接、供电和数据角色、PD 状态、mux 从 EC 只读读出，发布成 `CoachzTypeCPort` 服务并通知变化，两个口都记录到接入和拔出；30 W 充电器在系统的 `AdapterDetails` 里是 12 V、2.5 A，当前电压另外记录；连 Mac 不会被识别，USB 设备模式没做；U 盘插到两个 USB-C 口，内核的 USB 栈都能枚举到，但还没有发布成系统里的磁盘 |
| 内存 | 通过 | XNU 用上 4 GiB 地址以上连续的 6 GiB，约 5.83 GiB 可用 |
| SMP 和 CPU 调频 | 部分 | 八个核上线，IPI 正常，空闲核优先选 A76；按负载每 16 ms 调频（UniXNU 的规则），触摸和 GPU 工作时拉高；没有休眠，不是真正的大小核调度 |
| 温控 | 部分 | 25 路温度可读；超过 80 °C 降低 CPU 档位和 GPU 上限，自检时实测核频率：加上限后 A55 1516 MHz、A76 1563 MHz，和档位表一致；解除后 A55 回到 1804 MHz，A76 在 48 ms 内升到 1732 MHz，之后有没有回到顶档没单独测；`thermalmonitord` 读到 38 个 HID 温度服务（核心温度来自 TSENS，机身和电池类用电量计温度代替皮肤温度），日志里读了 700 多次，不再报找不到服务；它的决定还到不了限频，它找不到 CLPC 和 PMGR 服务，限频只由内核的 80 °C 规则决定 |
| 内存带宽 | 通过 | 按主线公式给 DDR 和内存总线投票后，内存延迟从约 195 ns 降到 135–140 ns，带宽和 Debian 一致 |
| 加速度计、陀螺仪、转屏、合盖、底座 | 部分 | 加速度计和陀螺仪 25 Hz，以 3 个 SPU HID 设备发布；面板横向扫描，由 `/chosen/display-rotation` 告诉系统，界面方向正确，转动机器时方向判断跟着变；磁吸盖走 iPadOS 原生的 Smart Cover 流程，合上熄屏、打开亮屏，这依赖写进 MobileGestalt 缓存的能力项，换成 iPad mini 身份的这版没带缓存，合盖只关背光；底座状态随拆装变化，不触发动作 |
| GPU 启动、Metal 合成界面 | 通过，还不够流畅 | 跑 UniXNU 当前的 Adreno740 框架，每个进程一张页表，进程的 GPU 地址和 Linux 一样从 4 GiB 起；CP 经 SMMU 执行命令，所有故障寄存器为 0；每次提交都完成；开了真实小组件和壁纸后提交到完成平均约 23 ms，部分帧超过 64 ms，繁忙时 GPU 100% 忙碌且大量时间等内存，很卡，原因在查；同机 Debian 上 UBWC 压缩是 760 和 330 FPS 的差别，但主屏的帧还没走上压缩纹理：主帧表面的像素正好占满最后一页，放不下标记，一帧都没用上；`backboardd` 和小组件进程都出现过 GPU 设备丢失，前者随后退出并带着 SpringBoard 重启，后者崩溃，原因没查明；fence 等待由中断唤醒，中断错误位全 0；空闲 66 ms 后 GPU 断电，下次提交时约 11 ms 恢复，界面正常；App 自己用 Metal 还没确认 |
| GPU 调频 | 通过 | 档位切换实测应答 18–60 µs、测得 179–800 MHz；按 UniXNU 的规则让 GPU 约一半时间忙碌，触摸后用最高档 |
| Wi-Fi、蜂窝网络 | 部分 | 照主线 `qcom_q6v5_mss` 启动 modem，AP 这边有 GLINK、QRTR 和 rmtfs，modem 一直运行；照 ath10k 走完 WLAN 固件握手、WCN3990 供电、12 个 copy engine、HTC 和 WMI 初始化，固件报告 READY；HTT 初始化后被动扫描能听到 2.4 和 5 GHz 的 beacon（只收不发）；系统里有 `en0`，设置显示 Wi-Fi 和周围的网络列表；系统发起加入后认证、关联和四次握手都完成，DHCP 和路由正常，Safari 能打开 HTTPS；开机后自动加入已保存的网络，状态栏信号图标正常（信号值是加入时的强度，还不会随距离变化）；蜂窝走主线 IPA：`ipa_config`、GSI、端点、命令通道、内存和路由表、LAN RX 都跑通，寄存器和 Debian 的真值一致，modem 接受 INIT_DRIVER，握手走到 INIT_COMPLETE，`ipa_modem_start` 后 `rmnet_ipa0` 打开，收发通道都已启动，Wi-Fi 不受影响；内核扮演苹果的基带接口（`AppleBaseband`、`AppleBasebandPCIControl`、sahara 和两路 qmux），`/baseband` 节点让 launchd 启动蜂窝版 CommCenter，根节点 `compatible` 用 J121AP 后 CommCenter 走高通基带分支，加载命令驱动并启动 ABM，状态栏显示搜索信号；发布 `AppleIPAppender` 和 PDP 管理器后 CommCenter 不再崩溃，设置里的“蜂窝网络”能打开，提示需要更新；固件目录按 ABM 要的路径 bind 挂载，BBUpdater 走完 BHI 和 sahara，ABM 报告基带启动成功并 engage，CommCenter 经 qmux 和真实的 modem 通信（53 个 QMI 客户端），系统识别到 SIM 卡，“关于本机”显示调制解调器固件；苹果私有 QMI 服务回不支持；modem 用 eMMC boot 分区里的原厂工厂数据（FSG）上线，“关于本机”显示 IMEI 和 ICCID，SIM 就绪；桥接层回应激活注册后 CommCenter 认为基带已激活并发起附着，数据口的 DPM 和 WDA 设置（raw IP、QMAPv4）被 modem 接受；插入有效 SIM 后在 LTE 上注册（漫游）并完成 CS、PS 附着，信号格传到状态栏，但界面仍显示无服务（CommCenter 认为基带服务未开通）；蜂窝数据和 eSIM 还没有通过 |

只看屏幕的阶段没有逐字段核对。有一个原因不明的随机 zone panic，没有解决；长时间稳定性没有验证。详情见 [状态](docs/status.md) 和 [硬件记录](docs/hardware-tests.md)。

## 站在谁的肩膀上

- **主线 Linux**：Coachz 是主线正式支持的整机，设备树和驱动给出了硬件的答案。Linux 是 GPL，这里只取事实和顺序，代码自己写。
- **[UniXNU](https://github.com/UniXNU)、UniXNU-sheng**：非 Apple 硬件跑 iPadOS 的框架，prelink、显示、触摸、音频和 GPU 的路线都来自这里。改编的文件保留了出处和协议。
- **ios-sweet**：屏幕控制台，以及系统启动问题的排错记录。

## 已有内容

- [原厂启动链研究](docs/native-boot.md)：固定到 Trogdor 固件维护分支，记录显示关闭、CPU 交接、内存与 FIT 格式的证据。
- [设计与阶段边界](docs/design.md)：比较直接 FIT shim、U-Boot EFI 与替换固件，明确首阶段的接口。
- `boot/`：带 Linux ARM64 Image 头的独立探针、EL2→EL1 入口、内存日志、受限 GENI UART 输出、只读 FDT 解析。
- `tools/`：交叉编译、FIT 封装、vboot 容器签名、上游版本核对；所有产物都是普通文件。
- `platform/`：`coachz` 是已验收检查点用的内核；`coachz-screen` 是不带 prelink 的屏幕内核；`coachz-prelink` 是主系统路线用的内核，所有驱动都在这里。
- [计划](docs/plans/)：GPU、Wi-Fi、音频等每个驱动的路线和真机上要看什么。
- [状态与真机验收](docs/status.md)：区分源码证据、主机检查、已通过的探针硬件验收与后续阶段。

## 构建

需要 Python 3、主机 C 编译器，以及 Zig 0.15.2。工具链可安装在项目内：

```sh
python3 -m pip install --target .tools/ziglang ziglang==0.15.2
python3 tools/build.py
python3 -m unittest discover -s tests -v
```

也可用环境变量 `ZIG` 指定已有的 Zig 可执行文件。

默认生成 memory、uart、reset-el2、reset-el1 四种探针：

| 产物 | 用途 |
| --- | --- |
| `build/memory/Image`、`probe.elf`、`probe.map` | 不访问外设的内存日志探针 |
| `build/uart/Image`、`probe.elf`、`probe.map` | 在原厂已经配置好 UART 的前提下，尝试输出日志 |
| `build/coachz-probe.dtb` | 仅供单核探针使用的最小 FIT 设备树；不是未来 XNU 的完整硬件描述 |
| `build/memory/probe.itb`、`build/uart/probe.itb` | 未签名 FIT；包含 Image 与探针设备树 |
| `build/manifest.json` | 实际使用的编译器、产物 SHA-256 与 Image 头信息 |

探针设备树的 `model` / `compatible` 是打包输入，不能当作真机身份探测结果。Depthcharge 会补入实际内存、原厂表位置等数据，探针记录这些交接信息。

构建对最终 ELF 的 AArch64 指令范围执行 [标量生成码检查](docs/codegen.md)，禁止早期探针依赖 FP／SIMD／SVE，并显式启用严格对齐。

## 无串口测试

见 [USB 测试步骤](docs/usb-test.md)。`reset-el2` 完成 EL2 诊断后等 3 秒复位；`reset-el1` 完成 EL1 诊断后等 10 秒复位。它们都不提供屏幕输出，观测到预期的成对延时重启只算粗粒度执行证据，不包含寄存器读数或 XNU 启动验证。

两种定时镜像观察到自动重启，普通内存对照与仅取消最终复位指令的匹配对照均不重启。单指令匹配对照为原版到达最终复位调用及此前 EL1／诊断门限提供间接硬件证据。后续 Linux 接收路线已取得直接寄存器日志；此前定时实验仍没有精确计时。见 [硬件记录](docs/hardware-tests.md)、[EL1 匹配对照](docs/control.md)与 [可读日志通道](docs/observability.md)。

在 Debian 构建／测试环境中，可使用已安装的 `futility` 和公共开发者测试密钥创建两个 64 MiB GPT 镜像：

```sh
python3 tools/prepare_usb.py
```

默认查找 `/usr/share/vboot/devkeys`、`/usr/share/vboot/devkeys-pkc` 和锁定的上游 `tests/devkeys`；也可用 `--key-dir` 指定。该命令只签名并创建文件，拒绝设备输出路径和覆盖已有文件。

## 签名

在具备 ChromiumOS `futility` 和开发者测试密钥的构建环境运行：

```sh
python3 tools/sign.py build/uart/probe.itb \
  --futility /path/to/futility --key-dir /path/to/devkeys \
  --output build/uart/probe.kpart
```

该命令仅创建并核验 vboot 容器，不写磁盘或固件。项目不提供自动刷写命令。将镜像放进可启动介质以及真机日志采集，是单独的硬件阶段。

## 当前可观测结果

探针在静态符号 `probe_log` 对应的内存中记录入口 EL、SCTLR、HCR、切换后的 EL、计时器频率、FDT 校验和交接属性；memory / uart 版本记录完成后停在 `WFE` 循环；定时复位版本在满足检查条件后请求重启。UART 版本只复用已初始化的 SC7180 QUPv3 UART，不下载固件，不改时钟或引脚；不可用时保留内存日志。

这一步不会恢复屏幕。已核对的 Depthcharge 源码会停止 timing engine、关闭背光并清零 DSI control，显示恢复需要独立验证；复用固件显示链的依据与待核对项见 [显示审计](docs/display-reuse-audit.md)。

## 原生 Linux 日志接收

已实现独立诊断路径：探针在 EL1 记录，受控 HVC 返回 EL2，交接保留的原生 Linux，在 chosen 设备树属性中读取日志。见 [构建与验收契约](docs/linux-log.md)。它使用现有原生 Linux 容器作为私有输入，不包含虚拟机，也不改变后续 iPadOS 引导目标；2026-10-27 已在真机启动并读回有效日志，确认 entry EL=2、current EL=1、stage=3、handoff EL=2。

## XNU 源码与装载前准备

下一阶段的源码入口、单核架构组件和主机装载检查见 [XNU 准备说明](docs/xnu-preparation.md)。`tools/kernel_source.py` 准备固定版本源码，`--build` 编译源码内核；`tools/xnu_prepare.py` 使用私有原厂 FDT／探针日志规划连续内存并生成最小 ADT，可选检查预链接 Mach-O 与 Darwin 23 boot_args。它们不生成新的可启动镜像，也不执行 XNU。

原生源码 XNU 的 pre-MMU 装载／boot_args 检查点见 [入口检查点](docs/xnu-checkpoint.md)。它已在真机通过单指令 HVC 返回 Linux 取回有效日志；后续 MMU、早期 C 参数复制、启动 CPU 数据及首次平台／ADT 查找检查点也已通过；后续平台初始化、平台驱动与 iPadOS 启动仍待完成。

[MMU 开启检查点](docs/xnu-mmu-checkpoint.md) 已通过真机日志验收：完整页表池、EL1 寄存器和硬件地址翻译校验成功，并返回 Linux。该轮只验证 MMU；随后高地址实际取指与栈读取由独立检查点确认。

[高虚拟地址与 C 入口检查点](docs/xnu-highva-checkpoint.md) 已通过真机验收：原始 RET 跳入 arm_init 高地址，完成栈帧调整与诊断读取后返回 Linux，未执行 C 初始化正文。

[早期 C 初始化与参数复制检查点](docs/xnu-cinit-checkpoint.md) 已通过真机日志验收：原始栈保存、完整参数复制、全局指针设置与缓存维护四个新字段及原有 11 字段全部通过，在 cpu_data_init 前返回 Linux；后续启动 CPU 数据及有限平台／ADT 查找的独立验收见下文。

[启动 CPU 数据初始化检查点](docs/xnu-cpu-checkpoint.md) 已通过真机验收：全部 16 字段有效，原始 cpu_data_init 的标量／FP/NEON 写入、完整 CPU 对象、保留哨兵与 guard 通过核对后返回 Linux。该有限检查点停在 PE_init_platform 前，不代表平台／ADT 初始化或完整系统启动。

[平台入口与原生 ADT 查找检查点](docs/xnu-platform-checkpoint.md) 已通过真机验收：完整 19 字段有效，正常 CPU 初值、原始平台状态与 ADT 绑定、原生 arm-io 查找及活动栈帧均通过核对后返回 Linux。检查点仍在后续 SoC 属性读取之前，后续平台／设备初始化与完整系统启动尚未验证。

[完整早期平台返回检查点](docs/xnu-pe-checkpoint.md) 已通过真机 22 个严格字段：完整 PE_init_platform(FALSE) 返回、SoC／型号／时钟结构／调试配置、父帧与缓存／页表返回均通过，随后进入 Linux。当前 ADT 缺失的总线／CPU 频率仍为零，clock info 只验证结构内容，不代表时钟驱动已支持。下一段 WFE／CPU 拓扑／线程／VM 的依赖和恢复契约见 [后续审计](docs/post-pe-audit.md)。

[SoC 属性诊断中点](docs/xnu-soc-checkpoint.md) 已由 version9 的真机日志验收：原始属性读取与缓存写入、全部 20 个严格字段、完整捕获／页表／硬件 AT 与缓存返回通过，随后交接 Linux。此前 version8 的无日志尝试保留为失败历史；这一步不覆盖后续 CPU／时钟／型号／调试配置或 SoC MMIO。

[同步异常与返回失败诊断](docs/xnu-diagnostics.md) 在修复包装器的绝对字符串指针缺陷后，已取得真机完整 BRK 异常记录并返回 Linux：精确故障位置／ESR、完整页表池与诊断传输均通过。此轮只验收异常回收通道，主动故障的 checkpoint=0 符合设计；随后 version9 的真实 SoC 属性路径也通过全部 20 个严格字段并正常返回 Linux。完整早期 PE 返回已在随后单独验收；完整系统启动、时钟／设备驱动仍未支持，主机验证证据单独保留。

[WFE 与单 CPU 拓扑检查点](docs/xnu-topology-checkpoint.md) 已实现 version11，126 项主机测试、七次系统模型回归和原生产物独立审查通过；检查三个系统寄存器的运行值和完整恢复，严格读取要求 40 字段。2026-10-31 真机完整 40 字段已通过，原始 WFE 配置／单 CPU 拓扑、完整状态核对与寄存器恢复后正常交接 Linux；不包含线程／VM 初始化。


[独立显示发现探针](docs/display-discovery.md) 已实现：校验 Coreboot framebuffer／GPIO 元数据，只读快照 13 个显示／GPIO／EC 协议寄存器，计算原 scanout 与整页分配范围的冲突，再 HVC 返回 Linux。137 项主机测试、11 次读异常 CPU 模型、旧 version11 回归及原生产物审查通过；2026-10-31 真机完整 42 字段通过，13 次读取无故障并正常交接 Linux。实际几何／格式匹配，候选为假的两个原因已经只读查明：当时的 1 MiB 基址门误读了分配器，复用的 XNU 放置范围门对动态 framebuffer 不可能为真，见 [核实记录](docs/display-discovery.md#2026-10-31真实分配与保留归属核实)。修正后的 schema 2 已通过真机 45 字段，候选为真。随后独立的画面实验恢复了 Depthcharge 停掉的 scanout：手电下可见全屏棋盘格，帧计数正好 60 Hz，并正常交接 Linux。随后的背光实验通过 EC 打开背光，不用手电即可看到棋盘格。发现探针本身不执行 XNU，不写 framebuffer、显示控制或 EC；候选标志不表示屏幕已点亮。

## XNU 屏幕输出与用户态

这部分镜像不再返回 Linux，结果都看屏幕。构建和配置见 [XNU 屏幕输出](docs/display-discovery.md#xnu-屏幕输出payload-version-12) 和 [prelink 屏幕镜像](docs/display-discovery.md#prelink-屏幕镜像)。

- 屏幕控制台改编自 ios-sweet，kprintf 和 panic 调用栈都画到屏幕上。
- 第一个用户程序是 `tools/coachz_init.S` 打包成的静态程序，由 mockfs 当作 `/sbin/launchd` 启动，每 5 秒打印一次 tick。
- 之后改用 IPSW 的 Update 恢复内存盘作为 APFS 根，运行 Apple 原版 launchd。内核每 60 秒打印一次存活信息和线程状态。
- SC7180 的虚拟定时器是 INTID 19，不是 ArmCpu 写死的 27，修正后定时器中断才开始工作。
- prelink 需要自己提供 IPSW。IPSW、kernelcache、合并后的内核和 GPU 固件都留在被忽略的目录里，不进仓库。
