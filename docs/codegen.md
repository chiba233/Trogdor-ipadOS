# MMU 关闭时的生成码约束

## 已确认的构建缺陷

首版定时复位探针的 C 编译参数包含 `-mgeneral-regs-only`，但 Zig 0.15.2 在 Clang 参数之后再次展开 `-mcpu=generic+el3`。`zig cc -###` 显示前面的 `-fp-armv8` / `-neon` 被后面的 `+fp-armv8` / `+neon` 覆盖，最终还关闭了 `strict-align`。

四种首版 ELF 的 FDT reservation map 扫描循环都包含向量访存与运算。以提交 f3ec9a2 的 reset-el2 为例，链接地址 0x2140 的机器码 0x3ced6800 是 `LDR Q0`，随后有 `EXT`、`ORR V0` 和 `FMOV`。这与没有配置 FP/SIMD 状态的早期探针契约不符。

另外，FDT reservation map 只保证 8 字节对齐，不能假设适合 16 字节向量读取。在 MMU 关闭且使用默认 Device 内存属性时，非对齐数据访问可能触发 alignment fault。

- [Arm 对启动代码和内存属性的说明](https://documentation-service.arm.com/static/63a43e333f28e5456434e18b)

## 修复

后端目标固定为：

```text
generic+el3+strict_align-fp_armv8-neon
```

`el3` 使汇编器接受 SMC 指令，不表示探针运行于 EL3。保留 `-mgeneral-regs-only`，并在后端明确禁用 FP/NEON、启用严格对齐。重新检查实际 Clang 参数，末次有效值为 `-fp-armv8`、`-neon` 和 `+strict-align`。

最终 ELF 在封装 FIT 前必须经过 `inspect_scalar_code`。该检查读取所有 AArch64 `$x` / `$d` 映射符号，只解读代码范围，拒绝 FP/SIMD 数据处理、向量 load/store 与 SVE 指令主类；缺失映射或范围不完整时失败。Image 头与内嵌数据不作为指令误判。这不是完整 AArch64 解码器，也不能证明任意运行期地址都合法。

## 验证边界

- 新检查实际拒绝了四种旧 ELF，均定位到 reservation map 中的向量加载。
- 修复后四种 ELF 通过机器码检查；FDT 字节读取使用严格对齐允许的标量访问。
- 回归测试覆盖向量访存、SIMD 运算、FP 寄存器传输、映射丢失和数据区不误判；17 项主机测试通过。
- 首次 EL2 外置启动观察为黑屏且无预期复位。构建缺陷已经确认，但缺少该次异常寄存器记录，因此不能宣称它已被证明是黑屏的唯一原因。修复版仍需真机复测。

原始固件日志、设备树转储和设备个体数据不进入仓库；可复现依据是固定工具链、构建参数与项目 ELF 的生成码。
