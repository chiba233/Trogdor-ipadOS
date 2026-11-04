# USB 主机：Apple 原版主机栈接到 DWC3

目标是让 iPadOS 自己的 USB 主机栈接管 usb_1 的 xHCI，外设按 iPadOS 原生方式工作：U 盘和移动硬盘、读卡器、键盘鼠标、hub、USB 网卡、USB 声卡、串口。现在的 libpayload 轮询栈只认一块存储和底座键盘、触控板，这一路接通后由 Apple 栈取代它。

## 设备模式做不到

`sc7180-trogdor.dtsi` 里 DWC3 是 `dr_mode = "host"`。Debian 上看到 SC7180 只有一个 DWC3（`a600000.usb`），xHCI 根集线器只有 1 个口，直接接板载 RTS5411（`0bda:5411`，`0bda:0411`），两个 USB-C 口和底座都在 RTS5411 的下行口上，`/sys/class/udc` 为空。集线器不能反向，所以这台机器连电脑时只能当主机，不能当设备。

## kernelcache 里有什么

IPSW 的 kernelcache（21H461，iPad7,x，T8011）带着整套主机栈：IOUSBHostFamily、`AppleUSBXHCI`、`AppleUSBHub`、IOUSBMassStorageDriver 加 IOSCSIBlockCommandsDevice、`IOUSBHostHIDDevice`、`usb.cdc`（ACM、ECM、NCM）、`usb.ethernet.asix`、`AppleUSBAudio`、`AppleUSBCardReader`、`usb.serial`。

真机上的匹配链是：

| 类 | kext | 匹配 |
| --- | --- | --- |
| `AppleT8011USBArbitrator` | AppleT8011USB | `usb-complex,t8011`，AppleARMIODevice |
| `AppleT8011USBPhy` | AppleT8011USB | `otgphyctrl,t8011`，AppleARMIODevice |
| `AppleEmbeddedUSBNub` | AppleEmbeddedUSB | 由仲裁器发布 |
| `AppleNubT8011USBXHCI` | AppleUSBHostT8011 | `usb-xhci,t8011`，AppleEmbeddedUSBNub，score 10000 |

仲裁器和 PHY 驱动操作的是 Apple 自家的 USB complex、PHY 和 PMGR，在 SC7180 上不能运行。

## AppleNubT8011USBXHCI 只是薄薄一层

`AppleNubT8011USBXHCI` 直接继承 `AppleUSBXHCI`（实例 0xe48 到 0xe68，多 4 个指针），`AppleUSBXHCI` 继承 IOUSBHostFamily 的控制器基类。两张 vtable（299 和 301 项）逐项对比，子类只覆盖了这些（名字来自 kernel 导出的 `IOService` vtable 和 `AppleUSBXHCI` 各函数日志里的函数名）：

| slot | 偏移 | 方法 | 子类多做的事 |
| --- | --- | --- | --- |
| 14 | 0x70 | `free` | 释放自己的锁 |
| 86 | 0x2b0 | `start` | provider 转成 `AppleEmbeddedUSBNub` 并保存（+0xe48）；读 `clock-mask`（service plane 向上找）生成时钟对象（+0xe50）；清 +0x494 的缓存模式位；再调父类 `start` |
| 132 | 0x420 | `message` | |
| 177 | 0x588 | 线程调用结束（停用） | 对 +0x4a0 调 `callPlatformFunction("setActive", false)`，时钟对象降到 10 |
| 180、182 | 0x5a0、0x5b0 | `lowerOnePowerStateTo` | nub 和时钟 |
| 183 | 0x5b8 | `raiseOnePowerStateTo` | 状态 1：nub 的 0x558 方法、时钟对象到 5、`setActive(true)`；然后父类 |
| 193 | 0x608 | 小钩子 | 转给 AppleEmbeddedUSBHost |
| 240 | 0x780 | 小钩子 | 一次 `ml_io_read32` |
| 273 | 0x888 | `createPort` | 按 ADT 的 `port` 子节点（`port-type`）用 `IODeviceMemory::withSubRange` 建可拔插端口 |
| 299、300 | 0x958、0x960 | 新增 | 加锁、解锁 |

xHCI 的命令环、事件环、端点、传输、hub 和端口状态机都在通用的 `AppleUSBXHCI` 和 IOUSBHostFamily 里。

## 设计

不用 Apple 的仲裁器、PHY 驱动和 `AppleNubT8011USBXHCI`，自己提供下面三样：

1. **CoachzUsbNub**：一个 IOService，代表 0x0a600000 的 xHCI。
   - `start` 里先按主线顺序跑现有的 `coachz_usb_soc_init()`（GDSC、时钟、QUSB2、QMP、DWC3 切到 host）。
   - 设备内存是 0x0a600000 起 0x100000 字节。
   - 中断是 SPI 133：重写 `registerInterrupt`、`enableInterrupt`、`disableInterrupt`、`getInterruptType`，经 ArmCpu 的 `armcpu_spi_attach` 接到 GIC。
2. **控制器对象**：一个 `AppleUSBXHCI` 实例，vtable 用 Apple 原表复制一份，只换平台 slot，做法和 `CoachzTemperatureDevice` 换 IOHIDEventDriver 的 slot 201 一样。
   - `start`：把 +0x494、+0x498 的缓存模式设为不缓存，让命令环、事件环和设备上下文不需要缓存维护，再调父类。
   - 电源相关的 slot（177、180、182、183）只调父类，DWC3 和 PHY 保持上电。
   - `createPort` 用父类的，按 xHCI 的 Supported Protocol 能力建 1 个 USB2 和 1 个 USB3 根口。
   - 先确认 `AppleUSBXHCI` 的 metaclass 能不能直接 `alloc`；不能的话，就分配 `AppleNubT8011USBXHCI`（大小足够）再换 vtable。
3. **CoachzUsbMapper**：给 nub 的 `IOMapper`，经 apps SMMU 给 USB 的流一个自己的上下文。
   - `AppleUSBHostController::start` 把 provider 存在 +0x88，+0x4a0 为空时调 slot 253，它就是 `IOMapper::copyMapperForDevice(provider)`；拿不到 mapper，start 失败。`copyMapperForDeviceWithIndex` 先看 provider 的 `iommu-parent` 属性，值本身是 IOMapper 就直接用，所以 nub 把 mapper 对象设成这个属性。
   - 有 mapper 时，`IODMACommand` 和 `IOMemoryDescriptor::dmaMap` 把整段描述符交给 `iovmMapMemory`，要它返回**一个连续的**设备地址区间。身份映射只对物理连续的缓冲区成立，U 盘这类分散的数据缓冲区做不到，所以要真的 IOVA。
   - 主线 `sc7180.dtsi` 里 DWC3 是 `iommus = <&apps_smmu 0x540 0>`，Debian 的设备树读出相同的值。固件留下的 apps SMMU（MMU-500，0x15000000）：sCR0 0x200001（CLIENTPD 置位，USFCFG 为 0，即没有匹配的流旁路），160 个 SMR 全部无效，80 个上下文银行，4 KiB 页。所以只加一个 SMR（0x540，mask 0），S2CR 指向一个自己的上下文银行，再清 CLIENTPD：其他所有流仍然没有匹配、照旧旁路，GPU、显示、Wi-Fi、音频、基带和 SDCC 不受影响。
   - 页表和映射用 ArmSmmu4K（GPU 已经在用）：`ArmSmmu4KLivePrepare` 建空的 arena，`iovmMapMemory` 按页批量映射（`ArmSmmu4KLiveUpdateContextBatch`），`iovmUnmapMemory` 解除并同步 TLB。IOVA 从一段固定窗口里分配。
   - DWC3 节点没有 `dma-coherent`，Linux 按非一致设备做缓存维护，这里也一样：map 时清，unmap 时作废。
   - XNU 是从源码编译的，IOMapper 的接口按源码写。

`AppleUSBXHCI::start` 对 provider 的要求（从反汇编读出）：
- provider 的 vtable +0x3c8 以 index 0 取设备内存，再 map 成 `IOMemoryMap`，存在 +0x4e8/+0x4f0；寄存器都经这个映射读。
- 中断：对 +0x88 的 provider 逐个 `getInterruptType(i)`（vtable +0x3f8），i 从 0 到 7，失败就停，每个建一个 `IOFilterInterruptEventSource`。nub 只给 index 0 就够。
- 命令环、事件环、设备上下文用 IOUSBHostFamily 自己的缓冲区分配，参数里带 +0x494（缓存模式）和 +0x4a0（mapper）。

libpayload 栈和 Apple 栈不能同时占用控制器。Apple 栈打开时（`coachz-usb-host=1`），内核不启动 libpayload 栈。

底座的输入只走一条路线，换栈只换传输：
- 路线：Zed 键盘和触控板进 `NativeHidInput`，report 1 是键盘，report 4 是顶排动作键（按 Zed 自己描述符的用法顺序），report 5 是触控板换算成的鼠标。解码在 `drivers/RamBlock/coachz_zed_input.c`，与传输无关；`coachz-usb-keyboard=1` 在两种栈下都表示走这条路线。
- libpayload 传输（`coachz_usb_hid.c`）：中断端点轮询，把报告交给这份解码。
- Apple 传输（`CoachzUsbHost.cpp` 的 `CoachzZedHidBridge`）：USB 一侧（枚举、管道、复位、电源）全部交给原版 `IOUSBHostHIDDevice`。桥接在它上面的 `IOHIDInterface` 这一层接手：personality 按 Zed 的 VID/PID（18d1:504c）匹配，探测分数 100000，高于 `IOHIDEventDriver`；probe 沿服务平面向上读 `IOUSBHostInterface` 的 `bInterfaceNumber`，0 是键盘、2 是触控板，其他接口让给原版驱动。start 经 vtable slot 168（`IOHIDInterface::open(client, options, action, refCon)`，`tools/record_usb_bindings.py` 核对：动作存在 +0x98，slot 181 `handleReport` 调它）带回调打开接口，原始报告交给同一份解码。系统只看到一份底座输入。
- 还没确认：`IOUSBHostHIDDevice` 是否让键盘留在 report protocol（12 字节，带动作键）；如果切到 boot protocol，报告只有 8 字节，日志会有 `8-byte keyboard reports, no action keys`，顶排键就要另想办法。

## prelink 依赖和 Wi-Fi 的类名冲突

prelinker 按 `OSBundleLibraries` 求闭包，每个原版指针的目标都要落在选中的模块里；原版段放在原地址加固定 slide（−0x1000000）处，加进新组件不会挪动已有组件，现有绑定地址不变。

xHCI、hub、composite 和 HID 四个根组件会多拉进 17 个 kext，其中一条链是 `AppleUSBXHCI` → IOAccessoryManager（24 个指针）→ AppleOnboardSerial（1 个）→ IOSkywalkFamily（49 个）→ IONetworkingFamily，切不断。USB 网卡驱动（`usb.cdc.ecm`、`usb.cdc.ncm`、`usb.ethernet.asix`、AppleUSBEthernetHost）本身也都依赖 IONetworkingFamily。

`CoachzWifiNet.cpp` 自己定义了 `IONetworkInterface`、`IONetworkStack`、`IONetworkStackUserClient` 三个类，原版 IONetworkingFamily 里也有同名的类，并且它的 `IONetworkStack` 会在 IOBSD 资源出现时挂到 IOResources 上启动。两边不能同时注册，所以接 Apple 主机栈之前，Wi-Fi 的注册表一侧要先改成建在原版 IONetworkingFamily 的类上。

`AppleUSBXHCI` 是抽象类（metaclass 的 `alloc` 返回 0），控制器对象要分配 `AppleNubT8011USBXHCI` 的实例，再换 vtable。`+0x494` 是缓冲区分配选项，`AppleUSBHostController::init` 给的初值是 0x713（RealTime）或 0x13；T8011 子类的 start 把缓存模式位（0xf00）清零，因为它的控制器是一致的。`+0x498` 的初值 0x20425355 是分配标签 `USB `，不是缓存模式。DWC3 不一致，所以缓存模式设成 write-combine（0x400，Normal 不缓存）；0x700（RealTime）在这份 XNU 里等于 Device 内存，不能用。

子类除了设计表里那些 slot，还覆盖了寄存器访问 289–297。它们只用传进来的寄存器窗口，和父类的逐条相同，只有 64 位写（293）拆成先低后高两次 32 位写，父类是一次 64 位访问；Linux 的 xHCI 驱动也是这样写 64 位寄存器（`lo_hi_writeq`）。所以这几个保留子类的，其余换回父类。

## 风险

- `AppleUSBXHCI::start` 里设备内存、中断和 mapper 三项已读出（见上）；电源树、`AppleUSBHostResources`、IOPortFamily 和 IOAccessoryManager 的依赖还没逐条读完。
- 本上下文的 ACTLR.CPRE（预取）清不掉，读回 0x3：安全侧的 CACHE_LOCK 锁着。主线在这台机器上也报 `Failed to disable prefetcher for errata workarounds, check SACR.CACHE_LOCK` 并照常运行。
- 清 apps SMMU 的 CLIENTPD 是全局开关：固件状态要在开机时重新读，任何一个 SMR 已有效就不动它，只记日志。
- xHCI 规范允许 64 位地址，DWC3 的 HCCPARAMS AC64 要在真机上读出来确认，内核内存都在 4 GiB 以上。
- 外设数据缓冲区的缓存维护全靠 mapper 被调到；IOUSBHostFamily 若有不经 `IODMACommand` 的路径，会出现脏数据，要用 U 盘读写比对来验证。
- iPadOS 的“文件”App 识别外接盘还要看 IOMedia 上的属性和用户态的文件系统服务，现在还没核实。

## 阶段

0. Wi-Fi 注册表一侧改建在原版 IONetworkingFamily 上，去掉自己定义的三个同名类。已在真机上通过：开机自动加入已保存的网络，DHCP 拿到 IPv4，IPv6 地址也拿到，DNS 可达，没有 `failed MACF`：
   - `tools/record_network_bindings.py` 在固定版本的 kernelcache 里按指令形状找出 `IONetworkInterface`（0x110 字节，221 个 slot，slot 178 纯虚）和 `IONetworkStack`（0xd0 字节，168 个 slot）的 metaclass、唯一的 vtable、保护构造函数和基类析构函数，核对 IONetworkingFamily 的指令 SHA 和两类对 IOService 的覆盖清单，按 −0x1000000 的 slide 写出 `package/OriginalIONetworkingABI.h`、`network-original-bindings.S`（60 个地址）和 `network-original.json`。
   - `CoachzWifiNet.cpp`：`IO80211Interface` 继承原版 `IONetworkInterface`，原版覆盖的 IOService slot 全部换回 IOService 的实现；`CoachzNetworkStack` 继承原版 `IONetworkStack`；注册自己的类之前先 `OSKextLoadKextWithIdentifier` 载入 IONetworkingFamily。configd 的沙盒只允许打开类名为 `IONetworkStackUserClient` 的用户客户端：换成别的类名时真机上报 `IOUC CoachzNetworkStackUserClient failed MACF`，InterfaceNamer 停住，wifid 卡在 `WiFiManagerCreate`，Wi-Fi 变灰。所以用户客户端用 `allocClassWithName` 分配原版实例，复制它的 vtable，原版覆盖的 IOUserClient slot（44、86、170、172、173）换成 `CoachzNetworkStackUserClient` 表里的实现，其余 slot 必须和 IOUserClient 相同，否则不建客户端。之后几次开机里 `the network stack was opened` 都没有出现，configd 似乎不再打开它，但这不影响加入和 DHCP。
   - `build-coachz-wifi-net.py` 解析编出的两张 vtable：原版覆盖的 slot 必须归我们的类，原版自己的 slot 必须指向记录的原版方法，slot 数恰好相等，所有原版导入都在绑定文件里；用户客户端那张表必须恰好 186 个 slot，只拥有 0、1、7、44、172。
   - `coachz-prelink-network-family.patch` 在 `IOKitBSDInit()` 发布 IOBSD 前从 IOCatalogue 去掉 `IOClass = IONetworkStack` 的 personality，打印 `COACHZ net: original IONetworkStack personalities N, removed 1`；`board.toml` 加 `--root com.apple.iokit.IONetworkingFamily`。
   - 真机上 `COACHZ net: original IONetworkStack personalities 1, removed 1`，两条载入失败的日志都没有出现。
1. 主机侧：nub、apps SMMU 上的 mapper 和控制器对象的创建，`coachz-usb-host=1` 默认关闭。源码已写，`CoachzUsbHost.cpp` 单独编译和归档检查通过，整个内核还没编，没上机：
   - `tools/record_usb_bindings.py` 在固定版本的 kernelcache 里核对两个类的 metaclass、父类、实例大小、slot 数、子类覆盖的 slot、64 位写的拆分、start 里对 +0x494 的读写，以及 provider 上用到的 IOService slot 121、127 就是 `getDeviceMemoryWithIndex` 和 `getInterruptType`，写出 `package/OriginalAppleUSBABI.h`、`usb-original-bindings.S`（原版 `AppleUSBXHCI` 表的地址）和 `usb-original.json`。
   - `package/CoachzUsbHost.cpp`：`CoachzUsbMapper`（ArmSmmu4K 建页表，IOVA 窗口 0x10000000 起 1 GiB）、`CoachzUsbNub`（寄存器窗口 0x0a600000、SPI 133、`iommu-parent`）、控制器对象（原版 personality 初始化，start 前把缓存模式设成 write-combine）。只在 eMMC 根路线上由 `NativeRamBlock::start` 启动，取代 libpayload 键盘栈。
   - `build-coachz-usb-host.py` 只引用 ArmSmmu4K（对象在 `libCoachzGpu.a` 里），`board.toml` 加四个 prelink 根（`AppleUSBHostT8011`、`AppleUSBHub`、`AppleUSBHostCompositeDevice`、`IOUSBHostHIDDevice`），闭包新增 22 个 kext。
   - 开关关着时启动和以前一样：新 kext 里只有 IOUSBHostFamily 的 `AppleUSBHostResources` 和 `AppleUSBHostResourcesTypeC` 会自己启动（挂 IOResources），`coachz-prelink-usb-host.patch` 在 `IOCatalogue::addDrivers` 里把它们去掉并打印 `COACHZ usb-host: ... left out`。
   - 镜像用 `tools/xnu_screen.py --root-emmc --usb-host`。
   - 提供者必须是电源节点：`changePowerStateTo` 只在节点位于电源平面且父节点已知状态时生效（`IOServicePM.cpp`）。nub 没做 PMinit 时，控制器 start 返回 1 并建好了环（10 次映射、2 个中断），但上电请求被丢掉，控制器始终没有运行，也没有端口事件。现在 nub 挂在根电源域下，注册两个状态并置为开；监视行带上控制器电源状态、USBCMD、USBSTS 和各口 PORTSC。
   - 缓存一致性：控制器不窥探（sc7180.dtsi 的 DWC3 没有 dma-coherent），原版驱动按一致的 T8011 写环、不做缓存维护。上机时开机建的结构都是写合并（VM_WIMG 0x6），上电后给每个设备分配的那一页却是可缓存的（0x2），第一次 EP0 传输两口都超时、事件环再无新事件，DWC3 和 SMMU 都没有记错误。现在 mapper 在映射期间把驱动自己的 IOBufferMemoryDescriptor 页改成写合并（相当于 dma_alloc_coherent），按物理页计数，取消映射后恢复；其他客户内存仍在映射和取消映射时清理和失效缓存。
   - 上机先看 mapper：`COACHZ usb-host: apps SMMU on`，其他设备照常工作，监视行里没有 FSR、sGFSR；再看 `controller start returned 1`、中断计数上涨、根口和 RTS5411 被枚举。
2. 真机第一张：控制器起来，根口、RTS5411 和底座被 `AppleUSBHub`、`IOUSBHostHIDDevice` 枚举，日志有 `base keyboard bridged into the keyboard device` 和 `base touchpad ...`，打字、顶排键、触控板和以前一样。
3. U 盘：IOUSBMassStorageDriver 发布块设备，读写比对通过，热插拔两个口。
4. 其他类：USB 网卡（ECM/NCM/ASIX）、声卡、读卡器、串口，逐类上机。

## 逆向工具

kernelcache 分析脚本不进仓库：按 bundle 列 personality、切出 kext 代码包成 ELF 交给 `llvm-objdump`，标注 adrp/add 指向的字符串和 kernel 导出符号，按 `getMetaClass` 认 vtable 并逐项比较。kernel 本体带 5967 个导出符号，kext 没有符号。


## 整体链接候选

固定 ARM64 缓存的 IOUSBHostFamily 引用 AppleSMC；AppleSMC 的实际指针还引用 RTBuddy、AppleARMPMU、AppleEmbeddedTempSensor，显式加入 prelink 根。包中的两项库声明 IOACPIFamily 和 watchdog 在该缓存中不存在；固定摘要守卫下只移除这两项元数据声明，所有可执行组件及实际指针、指令重定位检查继续执行。AppleUSBHostPlatformProperties 只有 x86_64 属性，没有 ARM64 personality 或可执行代码，作为无活动 ARM64 匹配的数据 bundle 保留，不提升外架构的属性。

IOCatalogue 中的 gIOClassKey 是 OSSharedPtr，桥接补丁用 get() 传入 OSDictionary。这些是整体构建阶段暴露的集成问题；不能据此宣称原生 USB 栈已在硬件运行。
