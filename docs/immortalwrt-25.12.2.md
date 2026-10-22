# ImmortalWrt 25.12.2 基线迁移

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

ECM30 继续只生成 RAM 启动产物。NAND 补丁适配 6.12 的 SPI-NAND API，
保持独立逻辑视图、芯片 ID 限制、WRITE ENABLE 拦截、硬件块保护和写入拒绝；
不提供刷写镜像。PHY 时钟、复位次序及 CH397 过滤补丁移至 `patches-6.12`。
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
ECM30单独使用 `configs/ebyte-ecm30.config`，只生成RAM启动及检查用rootfs产物，
保持8 KiB逻辑页、512 KiB逻辑块和NAND写入拒绝；完整构建后重新验证4G和双网口。

此前 ECM30 文档中的 6.18 硬件测试属于历史证据，不能视为 6.12 已通过实机
验证。ECM30 的完整构建及离线产物检查已完成，结果见下节；TAS 联合发布
构建和 ECM30 的 6.12 实机验证仍待完成。此次迁移不授权刷机或更换密钥。

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

构建源码为 `c8e7bf5e2cfd9d2316c6ee45c62b790a7e9569e5`，固件报告
`ImmortalWrt 25.12.2 r38146-c8e7bf5e2c`，内核为 `6.12.103`。
构建机从干净工作树拉取已推送提交，按上述固定 feeds 和 ECM30 seed 构建。
首次切换基线执行 `make dirclean`，重建 GCC 14.3.0、binutils 2.44 和 musl
工具链；后续修正仅清理相应包，再完成全量镜像构建。

构建中修正了两个发布版兼容问题：PPP 2.5.2 的包补丁移除针对不存在的
`dhcpv6relay/Makefile.am` 的修改，保留插件安装规则；移除上游源码归档遗留的
静态 `version`、`version.date`，使固件 revision 来自实际 Git 提交。
最终完整构建退出状态为 0，PPP 2.5.2-r5 的 `pppoe.so` 已进入 rootfs。

产物位于本地忽略目录 `artifacts/ebyte-t113i/ram-c8e7bf5e2c/`；完整归档为
`artifacts/ebyte-t113i/ecm30-release-c8e7bf5e2c.tar.gz`。

| 产物 | 字节数 | SHA-256 |
| --- | ---: | --- |
| `ecm30.uImage` | 9223372 | `fcdcff350a993fd04f7ee68abe889693ea6c96b3aceca3059491e22f46101156` |
| `ecm30.dtb` | 19452 | `0a854c5a22b2bf1642570a61992a57ecdaf2ed721affdb126bad781121468047` |
| `immortalwrt-sunxi-cortexa7-ebyte_ecm30-rootfs.tar.gz` | 5026307 | `0aedb4b2a83965c4c9597ba83acb53a4e4f3ea7c803ea461802938527a4bafa0` |

完整归档 SHA-256：
`5e138f317531cad52c53017d5bba34bd13ff4566e6c5b38295c5167736712a4d`。
归档内 18 个文件的 SHA-256 全部通过；镜像、manifest、seed、最终构建配置、
内核配置、feeds 版本、源码提交、构建日志及校验表一起保存。

已拆解最终 rootfs 和 initramfs，并检查：

- uImage 头及数据 CRC、独立 DTB 与附加 DTB 一致；加载和入口地址均为
  `0x43000000`，解压后内核末端 `0x41425b30` 低于 OP-TEE 起点 `0x41900000`。
- initramfs 含 1017 个条目，185 个 ARM ELF 的依赖均可解析；选中
  EdgeNode 0.3.70-r1、Telnet 客户端和服务端、tcpdump-mini、LuCI、UBI 工具。
- 最终镜像保留 ECM30 PHY 时钟及复位补丁、CH397 自定义 MAC 接收修正、
  `eth0 eth1` 的 LAN 桥接及 `usb0` 的 4G DHCP 配置。
- 防火墙保持 LAN 可管理、WAN 输入拒绝及 LAN 到 WAN 转发规则；4G 配置权限
  为 0600，没有发现内嵌私钥。未更换密钥或改变证书策略。
- NAND 仍为 8192 字节逻辑页、524288 字节逻辑擦除块；保留写使能拦截、
  写入拒绝和禁用刷写的升级入口。

检查结果见同目录的 `verification.json`、`preflight-output.json` 和
`release-sha256sums.txt`。

## 实机验证状态：等待串口恢复

准备 RAM 启动时，Windows 已无法枚举调试串口，用户确认暂时无法重新连接。
因此未执行这次 25.12.2 的 RAM 启动，也未重启或刷写板子。新内核的 4G
自动拨号、公网访问、两个网口收发、自定义桥接 MAC 和 NAND 只读数据对照
均仍待实测，不能使用上节离线检查代替。

构建期间在仍运行的旧 `6.18.44` RAM 系统完成了一次 NAND 只读基线复核：
sys 分区 1004 个逻辑擦除块，仅块 235 标记为坏块；10 组块首、块尾样本
均与原厂基线相符；MTD flags 为 0，UBI `ro_mode=1`，原厂 UBIFS 只读挂载可读。
boot 卷完整 SHA-256 为
`de32431458cd6f33f51d1b6c835f71be7d2a83a88ba928c9840b2abc3ebaded5`，
rootfs 首 4096 字节 SHA-256 为
`e82fd0713670de18a5f0e5db4a38f9863b0ca21ed312c1213955a1c3ab4851a3`。
日志位于 `artifacts/ebyte-t113i/25.12.2-prebuild/`。验证后已卸载 UBIFS 并
分离 UBI；这些结论仅对应旧内核。

准备验证新固件自动拨号时，旧 RAM 系统的 `4ginfo` 服务已停止，并执行临时
`AT+QNETDEVCTL=0` 断开数据连接。串口随后不可用，LAN 管理接口也未能登录，
因此尚未恢复 4G。可通过已有管理凭据在 LuCI「系统 → 启动项」启动
`4ginfo`，或在控制台执行 `/etc/init.d/4ginfo start`。
串口恢复后，先确认当前系统状态，再用已校验产物执行 RAM 启动和实测；
保持 NAND 只读，不执行 erase、write、saveenv 或 sysupgrade。
