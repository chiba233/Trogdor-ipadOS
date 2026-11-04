# 本地应用安装环境

此目录是尚待原生启动验收的候选。`tools/coachz_apps.py` 接收本地官方 Procursus rootless bootstrap、Sileo、TrollStore Lite 包及原版系统清单，生成环境归档、静态信任缓存及扩展 addon。第三方二进制不进入仓库。

## 直接启动任务

`tools/xnu_screen.py --apps` 要求 prelink、eMMC 根和 GPU mode 9，只增加 `/chosen/coachz-owner-apps`，不更改设备型号。内核在 Data 准备好后安装 addon，在第一次执行原版 launchd 之前建立只读 union：恢复 `/usr/libexec` 中两个原版程序，挂入安装任务 plist，再将新清单挂到 `/System/Library/xpc/launchd.plist`。三次挂载分别打印 stage/status；前一项未达到 stage5/status0 时停止后续准备。

新清单保留原版所有任务和其他元数据，仅新增 `com.coachz.apps`，由 launchd 直接以 root 启动 `coachz-apps bootstrap`。原版 mtmergeprops 仍是独立任务，sysstatuscheck 也不再触发安装。不会再由包装器 spawn 安装器，不通过注册器发送 XPC LOAD。addon 显式覆盖旧 wrapper 为原版字节，避免 Data 上残留的文件继续充当入口。

新 job 使用 `com.apple.private.security.no-sandbox = true`，拼写取自同一版原生清单的 replayd 任务；安装器本身保留已有 no-sandbox、no-container 和 skip-library-validation entitlement。没有把原始 SBPL 文本填入未核实的 SandboxProfileData 字段。这个声明在安装任务上的实际权限效果仍待实机验证，不等于开放整个系统的永久 root 模式；没有加入全局 MAC 或 AMFI 豁免。

此次入口需要重新构建内核：原内核仅提供旧的 `/usr/libexec` union，没有新增的启动清单挂载。配套 addon 与 apps TC 必须一起更换，并保留系统、GPU 等其他信任缓存。

## 持久状态

`kern.coachz_trustcache` 是显式启用镜像上的 root 写接口，接受有界 legacy v1 模块，由原版 TrustCache 库验证。每次启动扫描 `/var/jb` 和已安装应用的 CodeDirectory 并装载运行时缓存；dpkg 包装器在解包前装入包内摘要，ldid 包装器在重新签名后装入新摘要。原始 dpkg、ldid 保留为 `.coachz-real`。这不提供 FairPlay 解密。

首次解包与完整配置分别记录状态。相同 revision 不重复解包；配置成功后跳过 firmware、pwd_mkdb 和 dpkg 配置。失败重试不覆盖已提取的包数据库。当前安装器仍会在每次启动重跑两个应用的 uicache 注册，后续应收敛为首次成功或应用变更时执行。没有启动带默认密码的 SSH 服务，也没有安装 tweak injection 引擎。

`--reuse-environment` 指定已部署产物目录，安装器、环境归档和 revision 按原字节保留；输入和输出必须不同，并核对 revision，避免入口修改覆盖既有包状态。本地 assets 还需 launchd-stock.plist、原版 sysstatuscheck 和 mtmergeprops。构建重新解码最终 addon，核对每项资源、权限、恢复的两个原程序及 TC 覆盖；删除新增任务后，新清单必须与原版逐项完全相同。GPU 原条目的模式和压缩字节保持不变。

日志在 `/private/var/log/coachz-apps.log`。验收先确认三项 union stage5/status0，接着 installer entered、缓存装载成功、firmware 与包配置成功、两个应用注册并启动、实际安装的未加密 IPA 能启动，以及再次启动后的持久性。编译和打包不代表这些项目通过。

参考：

- [Procursus rootless bootstrap](https://apt.procurs.us/bootstraps/1900/)
- [Sileo 源码和发布](https://github.com/Sileo/Sileo)
- [TrollStore Lite 官方包](https://havoc.app/package/trollstorelite)
- [TrollStore root helper](https://github.com/opa334/TrollStore/blob/main/RootHelper/main.m)
- [UniXNU 移植文档](https://unixnu.com/zh/porting/)
