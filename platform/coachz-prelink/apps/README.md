# 本地应用安装环境

此目录是尚待原生启动验收的候选。`tools/coachz_apps.py` 接收本地官方 Procursus rootless bootstrap、Sileo、TrollStore Lite 包及原版系统清单，生成环境归档、静态信任缓存及扩展 addon。第三方二进制不进入仓库。

## 直接启动任务

`tools/xnu_screen.py --apps` 要求 prelink、eMMC 根和 GPU mode 9，只增加 `/chosen/coachz-owner-apps`，不更改设备型号。内核在 Data 准备好后安装 addon，在第一次执行原版 launchd 之前建立只读 union：恢复 `/usr/libexec` 中两个原版程序，挂入安装任务 plist，再将新清单挂到 `/System/Library/xpc/launchd.plist`。三次挂载分别打印 stage/status；前一项未达到 stage5/status0 时停止后续准备。xpc union 的第二项存在性检查直接核对 launchd.plist.sig。

新清单保留原版所有任务和其他元数据，仅新增 `com.coachz.apps`，由 launchd 直接以 root 启动 `/usr/libexec/coachz-apps bootstrap`。安装器通过现有 `/usr/libexec` union 提供，构建核对该入口的可执行模式、二进制字节和 TC 覆盖。原版 mtmergeprops 仍是独立任务，sysstatuscheck 也不再触发安装。不会再由包装器 spawn 安装器，不通过注册器发送 XPC LOAD。addon 显式覆盖旧 wrapper 为原版字节，避免 Data 上残留的文件继续充当入口。

直接任务在实机由 launchd 启动，运行时信任缓存装载成功，安装器仍以 134 退出。声明 no-sandbox entitlement 没有解决此前已捕获的 Data 可执行映射拒绝。

显式启用的镜像在三项 union 全部成功后允许标记安装器进程。exec 权限检查以目标 VFS context 的进程为准，仅在 root 执行指定安装器 vnode 时设标记；posix_spawn 和 vfork 不会把标记误写到 launchd。标记使用 proc 现有未使用字节的同布局别名，位于原生 p_forkcopy 区，fork、spawn 和 exec 的进程复制沿用原生继承。

带标记的进程及后代在 MAC 授权检查中跳过 Sandbox 策略，包括单独实现的 exec 检查和 mmap 权限降级；其他策略和标签生命周期照常执行。该豁免跟随安装器进程树，后代降权时仍保留。普通 root 进程没有此标记。已有信任缓存仍负责代码信任。

日志分别记录允许标记、安装器 PID 标记和第一次 exec、mmap、mmap 降级豁免，后续以 firmware、dpkg 与应用注册的实际返回值验收。标记、exec 与 mmap 豁免已在实机命中；该次运行的最后阶段是代码扫描开始，尚未取得包配置或应用注册结果。

此次入口需要重新构建内核：原内核仅提供旧的 `/usr/libexec` union，没有新增的启动清单挂载。配套 addon 与 apps TC 必须一起更换，并保留系统、GPU 等其他信任缓存。

## 启动清单的签名侧车

`launchd_cache_loader` 为清单加载同目录的 `launchd.plist.sig`，读取 CodeDirectory 并向 AMFI 查询其信任缓存状态，随后调用 `SecCodeMapMemory`。新清单若继续使用原版侧车，签名仍覆盖旧文件的长度与页摘要，读取映射时会触发代码签名页校验失败并终止 loader；仅校验 plist 格式和任务内容不足以交付。

构建为新清单生成 ad-hoc detached 签名，显式指定 SHA-256 和 4096 字节页大小，将 `.sig` 与清单放入同一个 xpc union，并把新 CodeDirectory 的摘要加入配套 apps TC。重新解码最终 addon 后核对文件长度、每一页的摘要和 TC 覆盖，同时运行主机 codesign 验证。已有清单 union 钩子的内核可复用，这个侧车修订只需更换 addon 和 apps TC。主机验证不代表原版 loader 已在实机接受新签名。

## 持久状态

`kern.coachz_trustcache` 是显式启用镜像上的 root 写接口，接受有界 legacy v1 模块，由原版 TrustCache 库验证。首次配置直接使用随镜像交付、覆盖整个 bootstrap 归档的静态缓存；配置完成后的启动扫描 `/var/jb` 和已安装应用的 CodeDirectory 并装载运行时缓存。扫描先读取魔数，只映射 Mach-O，并逐文件释放临时对象；dpkg 包装器在解包前装入包内摘要，ldid 包装器在重新签名后装入新摘要。原始 dpkg、ldid 保留为 `.coachz-real`。这不提供 FairPlay 解密。

首次解包与完整配置分别记录状态。相同 revision 不重复解包；配置成功后跳过 firmware、pwd_mkdb 和 dpkg 配置。包配置和界面登记分别记录完成状态，修复显示或登记不会重跑包配置。失败重试不覆盖已提取的包数据库。首次完整配置包含两个应用的 uicache 注册，完成 marker 成功写入后，再次启动只恢复信任缓存。安装器独立打开追加日志文件，并记录每个子进程的 spawn 和 wait 结果。没有启动带默认密码的 SSH 服务，也没有安装 tweak injection 引擎。

`--reuse-environment` 指定已部署产物目录，默认保留安装器、环境归档和 revision 原字节。配合 `--rebuild-helper` 可只修订安装器，保留归档和已部署 revision；归档摘要与源 manifest 核对。新安装器摘要加入静态缓存，解包 marker 和包数据库保持有效；输入和输出必须不同，并核对 revision，避免入口修改覆盖既有包状态。本地 assets 还需 launchd-stock.plist、原版 sysstatuscheck 和 mtmergeprops。构建在原生二进制清单的对象区之后追加安装任务，保留原对象字节和引用编号，并更新 LaunchDaemons 字典的对象表偏移。重新解码最终 addon，核对每项资源、权限、恢复的两个原程序及 TC 覆盖，删除新增任务后，新清单必须与原版逐项完全相同。GPU 原条目的模式和压缩字节保持不变。

日志在 `/private/var/log/coachz-apps.log`。验收先确认三项 union stage5/status0，接着 installer entered、缓存装载成功、firmware 与包配置成功、两个应用注册并启动、实际安装的未加密 IPA 能启动，以及再次启动后的持久性。编译和打包不代表这些项目通过。

参考：

- [Procursus rootless bootstrap](https://apt.procurs.us/bootstraps/1900/)
- [Sileo 源码和发布](https://github.com/Sileo/Sileo)
- [TrollStore Lite 官方包](https://havoc.app/package/trollstorelite)
- [TrollStore root helper](https://github.com/opa334/TrollStore/blob/main/RootHelper/main.m)
- [UniXNU 移植文档](https://unixnu.com/zh/porting/)

## 图标显示与登记检查

原生运行日志已经完成 firmware、密码数据库和包配置，两个应用的路径与标识也出现在 LaunchServices 持久主数据库中，界面仍未显示图标。uikittools 的 uicache 调用注册函数后没有把该函数的失败传给退出码，单独的退出 0 不作为登记验收。

安装器按 TrollStore root helper 的接口设置 mobile 账户的 `SBShowNonDefaultSystemApps`，同步并读回，记录前后状态。图标登记另外核对原生应用代理的 installed、placeholder、restricted 和 bundleURL；状态不满足时重试并保留失败结果，不写入界面完成标记。显示开关和应用代理检查的这批修订仍待实机验证。

- [TrollStore 显示开关调用](https://github.com/opa334/TrollStore/blob/88424f683b2a08f34a3f88985f790f97d84ce1df/RootHelper/main.m#L1240)
- [uikittools 注册调用和主程序退出处理](https://github.com/ProcursusTeam/uikittools-ng/blob/v2.1.7/uicache.m#L659)

## 应用启动和资源读取

两个应用已在实机显示，LaunchServices 代理报告 installed、非 placeholder、非 restricted，路径匹配。启动记录显示 xpcproxy 对应用主程序的 process-exec 被 Sandbox 拒绝并返回 EPERM，应用代码尚未执行；iconservicesagent 对同一目录的 file-read-data 也被拒绝。

运行权限候选沿用进程树标记，入口扩展到两个已部署应用的精确可执行路径，不改变执行身份。系统服务对 `/var/jb` 的读取、元数据、目录读取和只读映射增加同一 opt-in 下的 Sandbox 豁免，边界以已解析 vnode 的路径和目录分隔符判断；写入检查、Unix 权限、其他 MAC 策略、代码信任和标签生命周期保留。路径获取使用不回入文件系统的 vn_getpath，取不到路径时保留原有检查。图标登记版本更新以触发一次重新登记。该运行权限候选尚待实机打开应用验收。
