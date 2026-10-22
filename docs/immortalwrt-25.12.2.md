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

源码先在本地提交并推送，再由 `10.10.0.101` 的
`/home/openwrtbuild/immortalwrt-dtu` 拉取构建。迁移改变了内核、工具链及依赖
基线，首次正式构建需要清理旧输出；不可复用 6.18 的编译结果。
按照 AGENTS.md 更新固定 feeds、运行 `scripts/install-tas-feeds.sh`，复制
`configs/tastek-mt76x8.config` 后执行 `make defconfig` 和完整固件构建。

此前 ECM30 文档中的 6.18 硬件测试属于历史证据，不能视为 6.12 已通过实机
验证。完整固件、最终 rootfs、ELF 依赖、镜像大小与 SHA-256 仍需在正式构建后
验证。此次迁移不授权刷机或更换密钥。

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

日志保存在迁移工作区的 `build_dir/migration-validation/`。构建机
`10.10.0.101` 当时对 root 和 openwrtbuild 的 SSH 连接均直接关闭，未执行
完整发布构建或设备刷写。正式发布前仍须补齐上节的产物与实机验证。
