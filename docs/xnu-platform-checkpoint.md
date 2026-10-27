# XNU 平台入口与原生 ADT 查找检查点

version=6 沿原始 arm_init → PE_init_platform(FALSE) → SecureDTInit → pe_identify_machine → pe_arm_get_soc_base_phys → SecureDTFindEntry 调用链运行，在首次查找 name=arm-io 成功返回后停止。它验证平台状态初始化、ADT 绑定和真实树遍历；不进入 SoC 属性读取、MMIO、中断或 SMP 初始化。实现、91 项主机回归与独立审查已通过；外置签名镜像已完整回读，平台入口与原生 ADT 查找的真机执行仍待验收。

比较过仅在 SecureDTInit 后停止、直接运行整个 PE_init_platform 和在首次真实查找后停止。选择最后一种：不仅保存树指针，而且运行原生边界检查、递归遍历和 strcmp，同时仍停在机器识别后续读取与设备访问之前。补丁只替换 pe_arm_get_soc_base_phys 中该调用返回后的 MOV X8,X0，各旧模式保持独立。

## 输入与执行边界

移除本模式 CPU 哨兵，使用原始零初值，仍验证 cpu_data_init 的完整结果。保留 mode5 的受控 FPEN／ISB、arena 清理和 Linux 返回；CPU 哨兵模式继续仅供其自身诊断。

固定完整源内核及实际调用前缀／辅助函数哈希。PE 路径先初始化 PE_state／page_protection_type、复制零视频参数与像素格式字符串，再绑定 ADT。只接受最多 64 KiB、64 节点、深度 4 的有效 ADT，要求唯一且位于根下的 arm-io 节点，其首属性必须为规范 name。所有属性键及节点名 NUL 终止，防止 strcmp 越过字符串。返回门的目标节点和游标偏移由独立主机解析得到，不把原生解析器输出当作自己的期望值。

装载器在任何写入前验证全部新全局、CPU 对象及 guard、4 KiB 栈范围属于自有非执行内核段，与页表和原有区域分离；本轮涉及的初始状态均须为零。固定调用链的最大栈用量低于该范围，底部 64 字节 guard 应保持零。解析器只读 ADT，写入自己的游标和栈输出槽。

## 返回验收

新 SP 位于 arm_init 栈帧以下 0x130 字节的 SoC 查询帧。分别核对 arm_init、PE_init_platform、pe_identify_machine 和查询函数的完整活动栈帧；退休的递归遍历帧不要求为零。原 memmove 子帧槽现被活动 PE 调用保存的 FP／LR 合法覆盖，必须使用本模式的新期望值。

要求 x0=kSuccess、x19=const_boot_args、x20=FALSE、LR=精确查找调用返回 PC，FP/SP 与当前帧一致。核对完整 PE_state（包含像素格式／ADT／bootArgs）、page_protection_type、DT 根／大小／末尾／初始化标志、查找游标和输出节点指针；原有参数复制、正常 CPU 对象、完整表池和硬件 AT 仍须通过。

仅在完整 arena 缓存清理、全部内存／现场校验和状态恢复后交接 Linux。读取模式要求既有 CPU 检查点的全部 16 字段，加上平台状态、ADT 绑定与 ADT 查找三个字段。原始树、地址和日志保持私有；主机成功与真机执行分开记录。

## 验证计划

覆盖旧五模式、精确源码和单字补丁范围、树结构／节点／键与深度限制、写入前拒绝、全部 PE／DT 全局与活动帧破坏拒绝、CPU 正常初值与诊断模式分离、现场和严格日志门限、实际交叉编译及标量包装器检查。独立审查后生成签名普通文件，外置介质仍属性检查与全量回读，不自动重启。


## 构建与读取

```sh
python3 tools/xnu_checkpoint.py --kernel /path/to/source-kernel \
  --kernel-report build/xnu-source/source-preparation.json \
  --adt /path/to/generated.adt --output build/xnu-platform/payload.bin --platform
python3 tools/linux_log.py --source /path/to/native-linux.kpart \
  --futility /path/to/futility --key-dir /path/to/developer-keys \
  --xnu-checkpoint build/xnu-platform/payload.bin --output build/xnu-platform-usb
```

工具只生成普通文件。返回 Linux 后，以 `sudo python3 tools/read_probe.py --xnu-platform` 要求既有 CPU 检查点的全部 16 字段，以及三个唯一且值为 1 的新字段：

```text
XNU platform state verified=0x0000000000000001
XNU ADT binding verified=0x0000000000000001
XNU ADT lookup verified=0x0000000000000001
```

全部 19 字段通过后，仍须独立核对精确查找返回 PC、version=6、参数地址及负载清单。前一轮 CPU 初始化成功不能代替本轮原生平台／ADT 验收。

## 主机验证与介质准备

91 项主机回归通过，原有四种探针可构建。新用例覆盖规范树、节点／深度／大小限制、目标属性 NUL 后及对齐填充、写入前拒绝、全部新全局与活动栈字节破坏拒绝、退休调用帧允许保留、CPU 正常初值与旧哨兵分离、新现场、完整 19 字段门限、实际源码单字补丁和旧五模式兼容。实际交叉编译通过，包装器 5278 条指令保持标量约束。

独立审查复核原始调用链、四层活动帧与退休帧覆盖关系、原生解析器读写、最大约 0x3e0 字节栈用量、FP 使能、arena 清理及状态恢复，没有剩余阻断项。最终生成码确认 HVC 在原生查找调用返回处，后续 SoC 属性读取尚未执行；CPU 对象仅 mode5 保留诊断填充，mode6 使用原始零初值。

| 产物 | SHA-256 |
| --- | --- |
| source kernel | 6b406b8cd4ee5a6c11793256d8cadc5453365e8e8edb5f487953e02e259c7e9b |
| instrumented platform kernel | 96f3da91ac102bfd1892e0945572b16ffd718966017329191f2ca57352757ad8 |
| checkpoint payload | 7780d1a4a74a1dba08dc950219fda6e66d3aac9874fcc6661bb263f0589d6725 |
| linux-log.kpart | f87c09aa0b8c827107c902c9e7b579812868ea93b5333d84c2a822caf367c95e |
| linux-log.img | 5fef36cd41fc2f53890f89266bbb4f557f8ded4b0f12dd44b1bd2965d5b1f399 |

包装 Image 仍申请 74 MiB，Linux、47 份完整 DTB、initrd 和命令行保留。实际镜像主备 GPT CRC／分区属性／内嵌容器及代码清单匹配，主机与目标端 vboot 签名和 body 校验通过，传输哈希一致；固定哈希写入器完成外置介质全部 128 MiB 回读。没有写固件或内置盘，没有自动重启。原始二进制、ADT 和设备记录继续留在忽略目录。

本轮仍等待手动从原厂开发者模式启动并取回完整新日志。主机解析成功和此前 CPU 检查点通过，都不能证明本轮 native ADT 查找已执行；没有完整平台、驱动或 iPadOS 启动结果。
