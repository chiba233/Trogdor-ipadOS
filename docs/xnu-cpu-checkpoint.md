# XNU 启动 CPU 数据初始化检查点

沿用已验收的原厂 FIT → 原始 XNU → HVC → Linux 路线。version=5 放行原始 cpu_data_init 调用，在其返回后、PE_init_platform 之前停止。实现、82 项主机回归与独立审查已通过，外置签名镜像已完整回读；CPU 初始化真机执行仍待验收。

比较过只放行函数首条 SIMD、运行完整 cpu_data_init、以及直接运行 PE_init_platform。选择完整 CPU 数据初始化：该固定内核中的函数是没有子调用／栈调整／MMIO 的叶函数，全部写入限定在 BootCpuData 的 0x500 字节对象内，可以完整核对。平台初始化会引入 ADT 与更多调用，另行验收。

## 执行契约

固定完整源内核哈希，并增加 arm_init 到新检查点和完整 cpu_data_init 的指令哈希。BootCpuData 是 _percpu_slot_cpu_data 的源码别名；生成码接收其已链接 KVA，保存 RTClockData 指针及 CPU 信号／时间／睡眠等初值。本模式只把函数返回后的 MOV W0,#0 替换为 HVC，各旧模式继续独立打补丁。

该函数首次使用 FP/NEON，包含非 16 字节对齐的向量写入。引导器先保存原 CPACR，再仅为本模式设置 FPEN 并 ISB，冷启动仍执行原始 CPACR 写入；返回门要求实际 FPEN 和 SCTLR.A=0。引导器自身继续使用通过生成码检查的标量指令，原 CPACR 随原有 EL1 状态恢复；SIMD 寄存器为 Linux 内核入口未指定状态，不依赖其初值。

## 数据验收

写入前验证 CPU 对象及前后各 16 字节 guard 属于内核非执行段，初始全零，不与栈、页表或原有全局区重叠；RTClockData 地址也必须属于非执行段。完成全部输入校验后，仅本模式将 CPU 对象填为 0xa5，guard 保持零。该诊断输入只服务于有限检查点，不能直接用于后续完整 CPU／平台启动。

函数不读取对象原值，因此哨兵不影响该调用的控制流。原始生成码应清零的范围、五处非零常量／指针和未写字段都以独立字节模型核对；guard 不得变化。这样能观察原生 SIMD 清零，而不是把对象加载时的零值当作执行证据。

保留 mode4 的完整参数复制、BootArgs、父／子栈帧、页表与硬件 AT 校验；新现场要求 x0/x19 指向 CPU 对象、LR 指向精确调用返回 PC，不能沿用 mode4 的参数寄存器期望。叶函数不改变栈，memmove 子帧仍应保留其原返回地址。

仅对 mode4/5 执行完整 24 MiB arena 清理与 DSB，再关闭 EL1 M/C/I 并核对数据后返回 Linux。新增严格 CPU 数据字段；旧 C 初始化的 15 字段仍全部必需。主机测试覆盖精确源码、补丁范围、写入前拒绝、全部 CPU 字节与 guard 破坏拒绝、现场变化、日志门限和实际生成码。独立审查后生成签名普通文件，外置介质仍执行属性检查和完整回读，不自动重启。


## 构建与读取

```sh
python3 tools/xnu_checkpoint.py --kernel /path/to/source-kernel \
  --kernel-report build/xnu-source/source-preparation.json \
  --adt /path/to/generated.adt --output build/xnu-cpu/payload.bin --cpu-init
python3 tools/linux_log.py --source /path/to/native-linux.kpart \
  --futility /path/to/futility --key-dir /path/to/developer-keys \
  --xnu-checkpoint build/xnu-cpu/payload.bin --output build/xnu-cpu-usb
```

工具只生成普通文件。返回 Linux 后，以 `sudo python3 tools/read_probe.py --xnu-cpu` 要求既有 C 初始化的全部 15 字段，加上唯一且值为 1 的新字段：

```text
XNU CPU data verified=0x0000000000000001
```

完整 16 字段通过后，仍须独立核对精确返回 PC、version=5 负载、参数地址与源码清单，不能仅凭 Linux 返回或旧 C 初始化成功认定本轮通过。

## 主机验证与介质准备

82 项主机回归通过，四种原有探针可构建。新用例独立解释精确源码内核中 196 字节叶函数的指令，不与 C 验证器共享写入范围表，结果逐字节匹配；覆盖全部 1280 个对象字节和 32 个 guard 字节的逐一破坏拒绝、CPU 对齐／非执行段／初始值／重叠的写入前拒绝、现场与日志字段拒绝、旧四模式兼容和实际交叉编译。独立审查复核源码哈希、每条写入、FP 使能、非对齐访问、缓存恢复和各返回地址，没有剩余阻断项。

最终源内核保持 SHA-256 `6b406b8cd4ee5a6c11793256d8cadc5453365e8e8edb5f487953e02e259c7e9b`，仅在原始 CPU 调用返回后替换一条四字节指令；反汇编确认原始调用保留，PE_init_platform 尚未执行。引导器生成码确认仅本模式在源内核入口前写入 CPACR 并 ISB，自身 4660 条指令通过标量范围检查。

| 产物 | SHA-256 |
| --- | --- |
| instrumented CPU-init kernel | 920b96ae672d91ccf7c29cb5577049ded93d8a96b5001a19f81f75bd153bc2ee |
| checkpoint payload | 3c43733d2e9df158a683f50debdb43e1d45ba9384fe2e23f3b252ef14142e5b9 |
| linux-log.kpart | 898eb0b2065b8e9c2b122a334bb497576ee460822c69f1356a323f57d54aaf49 |
| linux-log.img | baae4bfebea2d3a7174be3f63785c058c0f47d9a3b3b89cfdfe90a7d68b1a1c3 |

包装 Image 仍申请 74 MiB，Linux、47 份完整 DTB、initrd 和命令行保留。实际镜像主备 GPT CRC／分区属性／内嵌容器通过，主机与目标端 vboot 密钥签名及 body 校验通过，传输哈希一致；固定哈希写入器已完成外置介质全部 128 MiB 回读。没有写入固件或内置盘，没有自动重启。原始源码二进制、ADT 和设备记录继续留在忽略目录。

非对齐访问的架构依据见 [Armv8-A memory model，第 12 章](https://developer.arm.com/-/media/Arm%20Developer%20Community/PDF/Learn%20the%20Architecture/Armv8-A%20memory%20model%20guide.pdf?revision=58b1dd0a-3800-4218-b21a-f95a0332034c)：Normal 内存允许非对齐访问，SCTLR.A 可使其触发异常；向量访存也遵循元素对齐规则。本模式固定完整启动页表和 MAIR，返回时新增检查 A=0；主机解释与生成码检查不能代替真机 SIMD 与缓存返回验收。

本轮仍等待手动从原厂开发者模式启动并取回新的完整日志。诊断哨兵不适用于继续运行平台初始化；下一步须移除该输入并为 PE_init_platform／ADT 单独建立契约。
