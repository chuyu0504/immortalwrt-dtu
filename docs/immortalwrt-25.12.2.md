# ImmortalWrt 25.12.2 基线迁移

ECM30 当前已安装 `25.12.2 r38159-6ddc3d5f35 / Linux 6.12.103`，从 NAND
自动启动。原厂 8 KiB / 512 KiB 布局、单一读写驱动、SquashFS＋UBIFS
OverlayFS、重启配置保持及保留配置 sysupgrade 已实机通过。最终产物和
验证范围见本文末尾；中间的只读与 RAM 记录保留为适配历史。

## 固定来源

- 固件基线：官方 `v25.12.2`，提交 `4fc16f2985a358bd43bb522e43f05395fcbd6ed5`。
- 内核：官方 Linux `6.12.103`，使用发布版的配置、工具链和通用补丁。
- packages：官方 `84bd86384928955b568988ca0e09e2c78c75173d`。
- LuCI：官方 `d6167ea0645cbd1327708d85f94824f42d0eb872`。
- 设备及应用定制来源：`06455bc6c9`，包含当时本地尚未提交的 root 构建支持和 ECM30 实机记录。

迁移分支直接从官方 tag 创建；不合并 master。feeds 使用发布 tag 中记录的
提交，而非会继续移动的 `openwrt-25.12` 分支。升级补丁版本时，应一起审查
固件 tag、内核补丁、两个 feed 提交及依赖版本。

仓库默认且唯一维护的分支为 `codex/immortalwrt-25.12.2`。旧 `main` 及开发分支
归档后移除，不再跟随或合并主线 master。GitHub Actions 的 push 构建也只监听
25.12.2 分支。分支清理前的完整 Git 历史及 stash 保存在本地忽略目录
`artifacts/git-backups/` 的 Git bundle 中；归档不作为继续开发的分支。

## 保留的功能

TAS-682F、IT-694_s3、EdgeNode 0.3.70、LuCI 应用、网络看门狗和既有精简策略
均保留。没有更改应用协议、平台配置、升级传输、证书策略和内嵌密钥。
HTTP/WS、HTTPS/WSS、多平台隔离、远程终端及网络恢复仍沿用现有实现。

ECM30 生成 NAND sysupgrade、Android boot 和 RAM 恢复产物。NAND 补丁适配
6.12 的 SPI-NAND API，保留原厂页/块映射、芯片 ID 限制及私有物理后端，
仅允许 sys 范围内的写入，保护前 10 MiB 的启动和安全存储分区。
PHY 时钟、复位次序及 CH397 过滤补丁移至 `patches-6.12`。
NAND 测试脚本从目标的内核版本文件读取版本及校验值。

发布版 libwebsockets 为 4.4.1；继续启用 libev、zlib、permessage-deflate，
其解析器已在用户回调前设置 inflater 完成状态，无须原 4.5.8 的回移补丁。
安装脚本检查版本，并仅清除内容与历史回移相同的残留补丁。固件版本显示
25.12.2，revision 使用实际
定制提交，避免误标为未经修改的官方二进制。

## 构建与验证

源码先在本地提交并推送，再由用户更正的 `10.10.0.100` 的
`/home/openwrtbuild/immortalwrt-dtu` 拉取构建。迁移改变了内核、工具链及依赖
基线，首次正式构建需要清理旧输出；不可复用 6.18 的编译结果。
按照 AGENTS.md 更新固定 feeds、运行 `scripts/install-tas-feeds.sh`，复制
`configs/tastek-mt76x8.config` 后执行 `make defconfig` 和完整固件构建。
ECM30 单独使用 `configs/ebyte-ecm30.config`，生成分离的内核、SquashFS、
标准 sysupgrade tar 及 RAM 恢复镜像，保持 8 KiB 逻辑页、512 KiB 逻辑块。

此前 ECM30 文档中的 6.18 硬件测试属于历史证据，不能视为 6.12 已通过实机
验证。ECM30 的完整构建、离线产物检查和本轮 RAM 功能验证已完成，结果见下节；
TAS 联合发布构建及 ECM30 长时间稳定性验证仍待完成。此次迁移不授权刷机或
更换密钥。

## 2026-10-22 本地验证结果

- EdgeNode 主机回归：24 项全部通过；应用源码与 `06455bc6c9` 完全相同。
- libwebsockets 4.4.1 使用 mbedTLS、libev 和 zlib 构建；真实 WebSocket
  线帧测试通过，覆盖普通传输、压缩及上下文复用。该测试不代表 HTTPS 证书实测。
- Linux 6.12.103 的 ECM30 NAND KUnit：9 项全部通过，包括写入拒绝；
  使用原版内核加本项目补丁，在 UML 中运行。
- 官方通用补丁及 ramips、sunxi 目标补丁应用通过，不依赖 fuzz。
- TAS-682F、IT-694_s3、ECM30 的设备树均通过预处理及 dtc 编译。
- TAS 联合发布配置与 ECM30 RAM 配置均通过 `make defconfig`，最终扫描无警告。
  保留官方 linux-firmware 包定义、预置 libuv 源码入口用于解析未选中驱动的
  依赖；没有因此在设备配置中启用这些固件或 libuv。

日志保存在迁移工作区的 `build_dir/migration-validation/`。最初尝试连接的
`10.10.0.101` 不可用；随后按用户更正的 `10.10.0.100` 完成下述 ECM30 构建。

## 2026-10-22 ECM30 完整构建及离线检查

构建源码为 `9059f835c6f9639c87131012501e8acbba1c500c`，固件报告
`ImmortalWrt 25.12.2 r38149-9059f835c6`，内核为 `6.12.103`。
构建机从干净工作树拉取已推送提交，按上述固定 feeds 和 ECM30 seed 构建。
首次切换基线执行 `make dirclean`，重建 GCC 14.3.0、binutils 2.44 和 musl
工具链；后续修正仅清理相应包，再完成全量镜像构建。

构建中修正了两个发布版兼容问题：PPP 2.5.2 的包补丁移除针对不存在的
`dhcpv6relay/Makefile.am` 的修改，保留插件安装规则；移除上游源码归档遗留的
静态 `version`、`version.date`，使固件 revision 来自实际 Git 提交。
最终完整构建退出状态为 0，PPP 2.5.2-r5 的 `pppoe.so` 已进入 rootfs。

产物位于本地忽略目录 `artifacts/ebyte-t113i/ram-9059f835c6/`；完整归档为
`artifacts/ebyte-t113i/ecm30-release-9059f835c6.tar.gz`。

| 产物 | 字节数 | SHA-256 |
| --- | ---: | --- |
| `ecm30.uImage` | 9233348 | `dc1da4fdd6d19ec3c7da44a5ffb986ae08388c138fb5b7014e3f9c7578a64eae` |
| `ecm30.dtb` | 19452 | `0a854c5a22b2bf1642570a61992a57ecdaf2ed721affdb126bad781121468047` |
| `immortalwrt-sunxi-cortexa7-ebyte_ecm30-rootfs.tar.gz` | 5026866 | `3feaf483000e1cb8191fdbabf25e46e3c482e64eae2248fbade8d40207e428a8` |

完整归档 SHA-256：
`a46863752ad4180ae9a5c465e3a0d57436f60672f352c2d5b0681e375dde9d62`。
归档内 18 个文件的 SHA-256 全部通过；镜像、manifest、seed、最终构建配置、
内核配置、feeds 版本、源码提交、构建日志及校验表一起保存。

已拆解最终 rootfs 和 initramfs，并检查：

- uImage 头及数据 CRC、独立 DTB 与附加 DTB 一致；加载和入口地址均为
  `0x43000000`，解压后内核末端 `0x41427bf0` 低于 OP-TEE 起点 `0x41900000`。
- initramfs 含 1017 个条目，185 个 ARM ELF 的依赖均可解析；选中
  EdgeNode 0.3.70-r1、Telnet 客户端和服务端、tcpdump-mini、LuCI、UBI 工具。
- 最终镜像保留 ECM30 PHY 时钟及复位补丁、CH397 自定义 MAC 接收修正、
  `eth0 eth1` 的 LAN 桥接及 `usb0` 的 4G DHCP 配置。
- 防火墙保持 LAN 可管理、WAN 输入拒绝及 LAN 到 WAN 转发规则；4G 配置权限
  为 0600，没有发现内嵌私钥。未更换密钥或改变证书策略。
- NAND 仍为 8192 字节逻辑页、524288 字节逻辑擦除块；保留写使能拦截、
  写入拒绝和禁用刷写的升级入口。
- 最终内核配置内置 D1 pinctrl、D1/R CCU、GPIO watchdog 及早期注册选项，
  并启用 6.12 所需的 RTC_CLASS、RTC_DRV_SUN6I 和 SUN6I_RTC_CCU。

检查结果见同目录的 `verification.json`、`preflight-output.json` 和
`release-sha256sums.txt`。

## 2026-10-22 RAM 实机验证

用户恢复 COM5 后，初版 `c8e7bf5e2c` 已尝试 RAM 启动：镜像和 DTB 的 RAM
CRC 正确，Linux 6.12.103 开始执行，但在用户空间启动前复位，随后回到原厂
Ubuntu。原厂 Ubuntu 的 rootfs 按原厂配置挂载为 rw；未执行镜像刷写命令。
核对最终内核配置发现 D1 pinctrl 和 RTC 时钟依赖缺失。缺少 pinctrl 会使
PG3 的 GPIO watchdog 无法探测；6.12 的 RTC CCU 由 rtc-sun6i 探测路径注册，
还必须启用 RTC_CLASS 和 RTC_DRV_SUN6I。已在上述新构建中补齐这些选项，并
显式关闭不适用的 CMOS 和旧 sun4i/sun7i RTC 驱动。

`9059f835c6` 已完成全量构建、扩充后的产物检查及 RAM 实机验证：

| 项目 | 实测结果 |
| --- | --- |
| 启动 | 实际运行 25.12.2 / 6.12.103，双核 online=0-1；物理架构定时器 24 MHz；D1 pinctrl 已探测 |
| 4G | EC200U 开机请求 PDP 1 的临时 ECM 连接，DHCP 获得地址、路由和运营商 DNS；经 usb0 的公网 ping 3/3、DNS 解析及 HTTP 204 请求通过 |
| 4G 恢复 | 临时执行 QNETDEVCTL=0 后，35 秒检查已自动重拨，QNETDEVCTL 返回 1,1,1,1，公网 ping 再次 3/3；监控实例运行且保留 procd respawn |
| eth0 | 原生 JLSemi PHY，100 Mbps 全双工；默认 MAC 和桥接 MAC 02:11:22:33:44:66 时电脑 ping 均 4/4，HTTP 有响应；连续四次关闭重开均返回 0 |
| eth1 | USB CH397 / cdc_ether，100 Mbps；桥接 MAC 分别为 02:11:22:33:44:66、02:11:22:33:44:55 时电脑 ping 均 4/4，HTTP 有响应；关闭重开后正常 |
| NAND | 四个 MTD 分区 flags 均为 0；sys 为 8192 字节页、524288 字节块、256 字节 OOB；1004 个逻辑块中仅 235 为坏块，UBI 无损坏块、8 个卷、ro_mode=1 |
| 数据对照 | 10 组块首/尾样本无差异；boot 卷完整哈希及 rootfs 首 4096 字节哈希均与原厂基线一致，UBIFS 只读挂载可读 |

USB CDC 的通用链路查询只填速率，不填 duplex 字段，因此本轮不以 sysfs
显示的 half 作为 CH397 的物理双工结论。HTTP 管理接口的未认证请求返回
403，本轮确认了 TCP/HTTP 收发，未更改管理凭据。

最终检查时本次 RAM 启动已连续运行 533 秒，未发现新的 Oops、panic、PHY
复位超时；这属于短时功能测试，不代表已经完成长时间稳定性或吞吐压力测试。
检查结束已卸载测试用 UBIFS 并分离 UBI。最终系统根目录和 /tmp 均为 tmpfs，
4G 已恢复联网，电脑线缆留在 eth1，桥接 MAC 为 02:11:22:33:44:55。
仅验证板子自身联网，未修改电脑网关或 DNS，未测试电脑经 4G 转发上网。

本轮原始日志及 `readonly-verification.json` 保存在
`artifacts/ebyte-t113i/ram-9059f835c6/`，包括启动、4G 自动连接及断线恢复、
两口自定义 MAC、NAND 基线对照及最终运行状态；测试没有刷写 NAND 或保存
U-Boot 环境。原厂 boot 卷及 rootfs 超级块哈希见下节，两次内核复核结果相同。

构建期间在仍运行的旧 `6.18.44` RAM 系统完成了一次 NAND 只读基线复核：
sys 分区 1004 个逻辑擦除块，仅块 235 标记为坏块；10 组块首、块尾样本
均与原厂基线相符；MTD flags 为 0，UBI `ro_mode=1`，原厂 UBIFS 只读挂载可读。
boot 卷完整 SHA-256 为
`de32431458cd6f33f51d1b6c835f71be7d2a83a88ba928c9840b2abc3ebaded5`，
rootfs 首 4096 字节 SHA-256 为
`e82fd0713670de18a5f0e5db4a38f9863b0ca21ed312c1213955a1c3ab4851a3`。
日志位于 `artifacts/ebyte-t113i/25.12.2-prebuild/`。验证后已卸载 UBIFS 并
分离 UBI；这些结论仅对应旧内核。

串口断开前临时停止旧 RAM 系统 4G 的状态已由本轮新系统联网及重拨测试解决。
截至上述 RAM 阶段的 NAND 测试保持只读，没有执行 erase、write、saveenv 或
sysupgrade。后续经用户授权实现读写并完成安装，见末尾的分离布局记录。

## NAND 启动镜像准备

已只读备份原厂 boot 卷到电脑，大小 17031168 字节，完整 SHA-256 为
`de32431458cd6f33f51d1b6c835f71be7d2a83a88ba928c9840b2abc3ebaded5`，
保存在 `artifacts/ebyte-t113i/nand-boot-9059f835c6/boot-original.bin`。
启动及安全存储分区不在更新范围内；后续分离安装替换了原厂 Ubuntu rootfs，
其完整备份已保存到电脑，可用于恢复。

`scripts/mk-ecm30-bootimg.py` 检查 uImage 头/数据 CRC、ARM zImage 边界、
ECM30 DTB 及容量限制，再生成适配原厂 U-Boot 的 Android v2 镜像及 JSON
校验报告。仅生成本地文件，不包含刷写操作。内核仍为已通过实机验证的
`9059f835c6`。原厂把启动镜像读到 0x43000000，Android 镜像将内核加载到
独立的 0x45000000，避免原厂 U-Boot 的镜像覆盖检查拒绝启动；原 uImage
的 RAM 入口仍为 0x43000000。Android 封装采用 2048 字节对齐，
这是镜像格式的对齐，与 NAND 的 8192 字节逻辑页分别处理。

内核组件保留已验证 uImage 中的附加 DTB，同时提供 Android v2 的独立 DTB
区段，使 ARM 解压入口使用自身匹配的 ECM30 设备树。

输出与 AOSP android-11.0.0_r1 的 mkbootimg 做逐字节比对，并验证损坏 uImage
和无效 DTB 会被拒绝。镜像大小和校验值随加载地址及附加 DTB 修正重新
生成，以最终 JSON 报告和实际通过 RAM 验证的镜像为准。
该文件是 boot 卷的内容，不是整个 NAND 或 sys 分区的原始镜像。

对照 SDK 的 U-Boot 源码发现，Android v2 路径会从 boot 卷重读 DTB，RAM
测试须使用临时的不存在分区名来避免读取原厂 DTB，再执行同格式启动。
该环境修改仅保留在 RAM，不保存环境。先完成此项验证，再使用原厂驱动
更新 boot 卷；原厂 Ubuntu 中已确认存在 ubiupdatevol 和 Python 3。

## 安装范围决策（历史）

用户在本轮 RAM 验证后明确要求优先实际落盘，并选择「先从 NAND 自动启动
ImmortalWrt，配置持久化随后做」。下一阶段先备份原厂 boot 卷，对照原厂
Android boot image v2 格式准备启动镜像，并在 RAM 验证同一镜像及原厂启动
参数后，仅更新 boot 卷。保留 boot0、U-Boot、OP-TEE、安全存储、环境及原厂
rootfs；继续提供串口 RAM 恢复入口。该阶段根文件系统仍来自 initramfs，
因此 /etc/config 的修改不会断电保存。

后续持久化按 OverlayFS 设计：SquashFS rootfs 作为只读下层，UBIFS
rootfs_data 作为可写上层。需要补齐 UBI block 支持和经过验证的 NAND 写路径，
再调整 sys 内的 UBI 卷；无需为了这个方案改动前面的启动及安全存储分区。
在现有只读驱动上不能直接创建或写入 rootfs_data。

长期联网及稳定性、吞吐压力，以及 RS485/串口、CAN、DI/DO 等工业接口验证
随后按用途安排。原厂 auto_test 作为接线和行为参考，不直接运行会切换网络
或输出状态的脚本。

## 2026-10-22 分离布局实机通过

用户随后将范围调整为直接分离内核、rootfs 和 rootfs_data，并要求一套完整的
原厂兼容 NAND 驱动，不区分只读、可写模式。临时 Android initramfs 镜像已成功
RAM 启动；内核仍为 9059f835c6，4G 公网 ping 3/3、DNS、eth1 HTTP 和看门狗
运行正常，但没有把该临时镜像写入 NAND。

新驱动保留同页双块拼接、16+16 字节受保护 OOB 和原厂坏块判定；写入拆成
4 KiB 物理页，擦除拆成相邻的两个 256 KiB 块。只注册逻辑 MTD，并在分区及
回调两层限制前 10 MiB 的写入。16 项 KUnit 测试通过，包括短写、半途失败、
跨块、4 KiB 子页、OOB、擦除及坏块标记。ARM 完整构建过程中还修正了擦除
边界检查的运行时 64 位除法，避免链接到内核不提供的 `__aeabi_uldivmod`。

采用固定 UBI 卷：boot 保留卷号 4、33 个 LEB；rootfs 保留卷号 5、使用
64 个 LEB（33030144 字节）保存 SquashFS；新增卷号 8 的 rootfs_data，
保存 UBIFS 上层。mbr、boot-resource、env、env-redund、private、UDISK 保留。
内核启用 UBI block，坏块预算使用 40/1024，与原厂 39 个可用坏块预留一致。

sysupgrade 接受带设备元数据的 ECM30 tar，检查 Android 内核、SquashFS、容量
和当前卷布局；更新 rootfs 与 boot 后分别回读校验，再重建 rootfs_data 并
恢复标准配置备份。拒绝裸 UBI 镜像，也不调用整个 sys 的 ubiformat。
10 项升级脚本测试通过。写入复用标准 `nand_upgrade_tar()`，板级准备函数
只检查既有布局并解除 ubiblock，避免通用的整片格式化回退。

最终源码为 `6ddc3d5f35b15dcc2bb3f658d92295d1446fde41`，完整构建退出 0。
后续 `bf7fe54549` 仅修正首次安装辅助脚本对 OpenWrt 可选 shell 变量的处理，
不改变已测试固件字节。RAM 恢复内核与 NAND 内核分别检查了 ELF 和 DTB；
正式内核只有 512 字节的空 initramfs，完整系统位于独立 SquashFS。
SquashFS 解包后的 670 个普通文件与 rootfs 归档一致，185 个 ELF 依赖通过。

### 最终布局

| UBI 卷 | 卷号 | LEB 数 | 容量 | 用途 |
| --- | ---: | ---: | ---: | --- |
| boot | 4 | 33 | 17031168 字节 | Android v2 内核与设备树 |
| rootfs | 5 | 64 | 33030144 字节 | SquashFS 只读下层 |
| rootfs_data | 8 | 856 | 441778176 字节 | UBIFS 可写上层 |

运行时 `/rom` 为 SquashFS，`/overlay` 为 UBIFS，`/` 为 OverlayFS。
UBIFS 报告约 387.9 MiB 文件系统容量，初始可用约 383.2 MiB；差额为文件系统
开销及预留，并非布局丢失。UBI 仍有 1003 个好块、1 个原有坏块，预留 39 个
坏块处理 PEB。

### 实机验证

- 原厂 boot 完整备份 17031168 字节，原厂 rootfs 完整备份 474808320 字节；
  后者 SHA-256 为 `d61d4d42c8705fc68d5a1fdd6e10fbc0a53556b61148e732b3b61ad446ea1cf9`，
  电脑副本与板端重新读取一致。其他小卷及启动分区哈希也已保存。
- 在 UDISK 空卷写入 516096 字节随机数据并回读一致，恢复原来的全 FF 内容；
  boot0、U-Boot、安全存储、环境、mbr、private 等保留区哈希一致。
- 首次安装由已验证的新 RAM 内核完成；内核和 rootfs 分别回读 SHA-256。
  SquashFS 挂载、UBIFS 写入/卸载/重新挂载通过，然后正常重启从 NAND 启动。
- 在 `/etc/config` 创建测试配置，普通重启后值仍在；随后对同一固件执行标准
  `sysupgrade` 保留配置升级。日志出现 `configuration saved` 和
  `sysupgrade successful`，重启后配置值再次核对通过，测试文件已清理。
- 升级后内核、rootfs 再次回读一致；全部保留区再次核对一致。双核在线，
  看门狗运行；4G 公网 ping 3/3、DNS、电脑网口 ping 4/4 和 HTTP 200 通过。
  当前接线口为 eth0；eth1 保留此前同驱动的 RAM 实测结果。

上述重启均为软件重启，未据此声称完成断电循环、长时间稳定性或吞吐压力测试。
原厂 U-Boot 仍打印其旧设备树接口及内存保留区告警，Linux 启动和联网不受本轮
测试中这些告警影响；本轮未修改 U-Boot，也不把 U-Boot 的网络下载作为已验证
功能。串口 RAM 恢复入口已保留。

### 产物

位于本地忽略目录 `artifacts/ebyte-t113i/nand-6ddc3d5f35/`：

| 文件 | 字节数 | SHA-256 |
| --- | ---: | --- |
| `boot.img` | 4249600 | `8a0401147adf66efccb587631008f028aded75c71376008128f2d1ff54873bbe` |
| `rootfs.squashfs` | 3853312 | `1c557174dd9263b0303ad4c4484e1a9eebbf7162b63beb22602db4936e5920ec` |
| `immortalwrt-sunxi-cortexa7-ebyte_ecm30-squashfs-sysupgrade.bin` | 8110344 | `00161df82bbf3a4da17b32183f9139aa9981c6595a02111fb20a22ac9ad21370` |
| `ecm30.uImage`（RAM 恢复） | 9236532 | `3b03403292174d3bd5446676e045ac682275d7c9305afaee6b288c2dd9cd35f6` |

首次安装脚本为 `scripts/ecm30-install-nand.sh`，默认要求显式选择检查或安装，
只接受备份对应的原厂布局。已安装设备后续使用上表的 sysupgrade 文件，
不要把它作为整个 NAND 的原始镜像或 PhoenixSuit 镜像使用。
