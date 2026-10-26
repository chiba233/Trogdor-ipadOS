# 仅抑制最终复位的 EL1 匹配对照

## 要检验的区别

`memory` 对照保持黑屏而 `reset-el1` 自动重启，仍不能唯一定位复位来源：前者还移除了 PSCI_VERSION 查询、计数器等待，且代码布局不同。

本对照从已有的 `reset-el1` 编译产物派生，直接把 checkpoint_run 中最后一条 SMC #0 替换为等长 NOP；不重新编译，不改变入口、目标 EL、FDT 诊断、PSCI_VERSION、10 秒等待、栈或其他 Image 字节。执行越过 NOP 后沿既有返回路径停在 WFE。内存 stage 会按原来的意外返回路径写为 5，不能将其解释为实际发出了复位请求。

这是诊断产物，不是启动缺陷的修复。

## 生成与校验

```sh
python3 tools/build.py
python3 -m unittest discover -s tests -v
python3 tools/control.py
python3 tools/sign.py build/control-el1/probe.itb \
  --futility /path/to/futility --key-dir /path/to/devkeys \
  --output build/usb-control/reset-el1-no-reset.kpart
python3 tools/disk.py build/usb-control/reset-el1-no-reset.kpart \
  build/usb-control/reset-el1-no-reset.img --label el1-reset-inhibited
```

上述命令只创建普通文件，不打开测试介质。按实际安装替换签名工具和公开开发者测试密钥目录；输出目录／文件必须尚未存在。

`control.py` 校验源 ELF、Image、FIT 的构建清单哈希、ELF 提取结果、标量指令范围及 FIT 内嵌 Image，再根据 ELF 中 checkpoint_run 的符号范围定位指令。它要求匹配固定工具链生成的两组 PSCI 指令序列，包含 PSCI_VERSION 和 SYSTEM_RESET 函数号及参数设置；数量、顺序或范围不同就拒绝生成。工具对新编译器或陌生生成码不作猜测。

生成后的 Image 只有一个 4 字节指令字改变；FIT 只有 kernel data 与其 SHA-256 属性改变，设备树及配置保持相同。输出 manifest.json 记录源构建清单／ELF／Image／FIT 哈希、补丁偏移、前后字节、输出哈希及未执行的硬件状态。派生 Image 不能冒充原始 ELF 的未经修改提取结果。

## 本轮已准备产物

源引导代码为提交 4054f94。原 Image 的 SHA-256 为 `ed562b0a31301909bd7f41b75a71acccf7aa65222fcb50b2c29de0961fd356cd`。

| 项目 | 值 |
| --- | --- |
| Image 偏移 | 0x272c |
| 原始指令字节，小端 | 03 00 00 d4（SMC #0） |
| 替换指令字节，小端 | 1f 20 03 d5（NOP） |
| Image 长度 | 原版／对照均为 10479 字节 |
| 对照 Image SHA-256 | 3fc7c65b82d7b2cb5ee8983a8bac69d6c257215cb75c8d33f888f6345d517474 |
| reset-el1-no-reset.img SHA-256 | 03d8e18a0788ff9581d8da33f8e5748ba03d18c92655a30bfe0f7a2e6559fa0c |
| reset-el1-no-reset.kpart SHA-256 | fd2cfa75379f9ac18b8375330d098744d104a7c80f09d2ed5e2d17d3f68ccc37 |

21 项主机回归通过；独立检查确认只有上述四个 Image 字节变化，以及两处预期的 FIT 属性变化。签名／body 校验与容器、GPT 中的负载字节一致性检查通过。目标系统的 futility 再次验证签名与 body 成功，写入外置测试介质后的全部 64 MiB 回读 SHA-256 与上述镜像一致。尚未执行该匹配对照的硬件启动测试。

## 执行与解释

从原厂开发者菜单选择外置启动，观察 30 秒，记录是否自动重启；若保持黑屏，随后可手动恢复内置启动。不要把手动恢复计作自动复位。

- 原版自动重启、匹配对照不重启，会为最终 SYSTEM_RESET 触发复位提供更直接的证据，也支持原版到达了此前的 EL／stage／计时检查；仍不提供寄存器日志，也不是 XNU 或 iPadOS 启动证明。
- 若匹配对照也自动重启，应继续分离 PSCI_VERSION 与其他复位路径；不能把它判为最终 SYSTEM_RESET 成功。
- 黑屏保持本身仍可能来自更早的停机或异常，因此不单独作为 EL1 检查点通过依据。
