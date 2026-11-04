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
3. **CoachzUsbMapper**：给 nub 的身份映射 `IOMapper`。
   - IOUSBHostFamily 用 `IOMapper::copyMapperForDevice` 取 mapper，再用 `IODMACommand` 准备外设的数据缓冲区。
   - SC7180 的 DWC3 节点没有 `dma-coherent`，Linux 按非一致设备做缓存维护，这里也一样：map 时清，unmap 时作废，设备看到的就是物理地址，apps SMMU 现在是 bypass。
   - XNU 是从源码编译的，IOMapper 的接口按源码写。

libpayload 栈和 Apple 栈不能同时占用控制器。Apple 栈打开时（新开关，暂定 `coachz-usb-host=1`），`coachz_usb_open_keyboard()` 不再调用 `xhci_init()`，键盘和触控板改走 `IOUSBHostHIDDevice` 和 IOHIDFamily。底座顶排键现在由 `coachz_usb_hid.c` 合成的 report 4 提供，换栈后要重新确认，必要时给它做一份 HID 描述符补丁。

## prelink 依赖和 Wi-Fi 的类名冲突

prelinker 按 `OSBundleLibraries` 求闭包，每个原版指针的目标都要落在选中的模块里；原版段放在原地址加固定 slide（−0x1000000）处，加进新组件不会挪动已有组件，现有绑定地址不变。

xHCI、hub、composite 和 HID 四个根组件会多拉进 17 个 kext，其中一条链是 `AppleUSBXHCI` → IOAccessoryManager（24 个指针）→ AppleOnboardSerial（1 个）→ IOSkywalkFamily（49 个）→ IONetworkingFamily，切不断。USB 网卡驱动（`usb.cdc.ecm`、`usb.cdc.ncm`、`usb.ethernet.asix`、AppleUSBEthernetHost）本身也都依赖 IONetworkingFamily。

`CoachzWifiNet.cpp` 自己定义了 `IONetworkInterface`、`IONetworkStack`、`IONetworkStackUserClient` 三个类，原版 IONetworkingFamily 里也有同名的类，并且它的 `IONetworkStack` 会在 IOBSD 资源出现时挂到 IOResources 上启动。两边不能同时注册，所以接 Apple 主机栈之前，Wi-Fi 的注册表一侧要先改成建在原版 IONetworkingFamily 的类上。

`AppleUSBXHCI` 是抽象类（metaclass 的 `alloc` 返回 0），控制器对象要分配 `AppleNubT8011USBXHCI` 的实例，再换 vtable。父类给 `+0x494` 的初值是：有 `kHasUCNormalMemory` 时 0x713（RealTime），没有时 0x13；这台 XNU 没定义 `HAS_UCNORMAL_MEM`，所以是 0x13，`+0x498` 为 0。

## 风险

- `AppleUSBXHCI::start` 和控制器基类对 provider 的其他要求还没逐条读完，比如属性、电源树、`AppleUSBHostResources`、IOPortFamily 和 IOAccessoryManager 的依赖。
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
1. 主机侧：写 nub、mapper 和控制器对象的创建，跑通编译和 kext 绑定检查，`coachz-usb-host=1` 默认关闭。
2. 真机第一张：控制器起来，根口、RTS5411 和底座键盘被 `AppleUSBHub`、`IOUSBHostHIDDevice` 枚举并能打字。
3. U 盘：IOUSBMassStorageDriver 发布块设备，读写比对通过，热插拔两个口。
4. 其他类：USB 网卡（ECM/NCM/ASIX）、声卡、读卡器、串口，逐类上机。

## 逆向工具

kernelcache 分析脚本不进仓库：按 bundle 列 personality、切出 kext 代码包成 ELF 交给 `llvm-objdump`，标注 adrp/add 指向的字符串和 kernel 导出符号，按 `getMetaClass` 认 vtable 并逐项比较。kernel 本体带 5967 个导出符号，kext 没有符号。
