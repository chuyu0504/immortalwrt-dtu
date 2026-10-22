# Ebyte T113-I ECM30 适配记录

> 当前源码已迁移至 ImmortalWrt 25.12.2 / Linux 6.12.103，见
> [基线迁移记录](immortalwrt-25.12.2.md)。25.12.2 已完成 NAND 安装和自动启动，
> 使用原厂 8 KiB / 512 KiB 布局、SquashFS＋UBIFS OverlayFS；重启配置保持和
> 保留配置 sysupgrade 已实测通过。RAM 恢复、4G 及网口验证记录见该文档。
> 下文 6.18 的测试结论仅对应历史产物，当前 6.12 的结果见上述迁移记录。

历史状态（6.18）：Linux6.18.44只读NAND兼容层已通过9项KUnit测试及实机数据对照，
保留8192字节逻辑页、524288字节逻辑块。ARM完整RAM固件已构建并通过
产物检查；按原厂补齐晶振和物理定时器配置后，已双核进入用户空间。
已定位持续运行复位来自断网300秒的软件重启策略，修正后RAM系统已持续运行超过此窗口。
4G ECM已实测获得地址、解析DNS及访问公网HTTP，开机拨号和断线恢复通过。
原生网口及USB网口均已在RAM系统完成单口收发，两个端口均可使用自定义桥接MAC。
完整镜像复测发现的PHY重复打开问题已完成模块实测修正，正在归档最终构建。
工业串口仍待验证。
不能把NAND只读数据一致视为固件已稳定。
不能用于 PhoenixSuit 烧录或 NAND sysupgrade。

## 证据

2026-10-21 通过 COM5 / 115200 读取原厂 Ubuntu 的设备树、dmesg、
MTD、GPIO、USB 和 AT 状态。实机系统时钟停留在 10 月 13 日，日志中的
系统时间不能用作检查日期。

原始镜像：`t113_i_linux_ebyte_t113i_ECM30_uart0_ubi_gd.img`

- 大小：357805056 字节。
- SHA-256：`90d74c60ff1d069801d8c0b2c0e59651f9dc84a35a1e20750a281bf025dd9b5f`。
- 格式：IMAGEWTY 0x300，33 个条目，包含重复的 env 条目。
- 板级标识：`t113_i-ebyte_t113i_ECM30`。
- 实机内核：`5.4.61-rt37`；原厂 U-Boot：`2018.07-00005-gb2540ae-dirty`。
- 两核 Cortex-A7，512 MiB DDR；内核报告 MemTotal 491284 KiB。

可重现镜像拆解（仅解析数据，不运行镜像中的文件）：

```sh
python3 scripts/extract-sunxi-image.py /path/to/vendor.img --output /path/to/new-directory
dtc -I dtb -O dts -o vendor.dts /path/to/new-directory/005-sunxi.fex
```

完整原厂日志和拆解内容留在本地忽略目录 `artifacts/ebyte-t113i/`，
没有把原厂大镜像、设备身份或密钥加入源码。

## 已确认的接线

| 功能 | 接线或实机信息 | 当前适配 |
| --- | --- | --- |
| UART0 / 调试口 | PG17 / PG18，115200 | `ttyS0`，earlycon |
| UART1 | PG6 / PG7；原厂 RS485 控制 PG13 | UART；RS485 控制待验证 |
| UART2 | PB0 / PB1 | UART |
| UART3 | PG0 / PG1；原厂 RS485 控制 PG2 | UART；RS485 控制待验证 |
| UART4 | PB2 / PB3 | UART；不能同时启用同脚 CAN0 |
| UART5 | PG4 / PG5 | UART |
| 原生网口 | RMII PE0–PE6、PE8、PE9；PHY ID 937c4024 | PHY地址0；供给50MHz参考时钟后100Mbps全双工收发通过 |
| PHY 复位 | 实机驱动占用 PE7，低有效 | MDIO总线复位，低50ms / 高50ms |
| USB HOST1 | QinHeng 1a86:8091 hub | EHCI1 / OHCI1 |
| 第二网口 | QinHeng CH397 1a86:5397，原厂 cdc_ether / eth1 | CDC Ethernet；与eth0桥接LAN，自定义桥接MAC收发通过 |
| 4G | Quectel EC200U，2c7c:0901 | option、CDC ECM / usb0；数据联网已验证 |
| 4G AT | ttyUSB0；原厂 ModemManager 还占用 ttyUSB5 | EdgeNode 使用 ttyUSB0 |
| LED | PG16 低有效、PG11 低有效、PB9 高有效 | heartbeat / user-base / user-ext |
| 用户按键 | PD15，低有效，KEY_PROG1 | 主线hotplug不支持此键码，待调整 |
| 外部看门狗 | PG3，level 算法，1600 ms | 内置驱动、early init、always-running |

原厂 DTS 同时写了 PHY 地址 1 / PE13 和旧式 `phy-rst=PE7`，与实机使用
地址 0、占用 PE7 的结果有差异。适配采用实机值，不能仅照抄静态 DTS。
原生 RMII 组特意排除 PE7，以免与复位 GPIO 争用。

原厂 `/dev/ttyAS*` 是 BSP 驱动名称；主线使用 `/dev/ttyS*`。现阶段不能
承诺工业串口已经支持收发：必须先确认 PG13 / PG2 是方向控制还是模式选择，
补齐相应主线 RS485 支持并做外部回环/总线测试。RAM 配置关闭 EdgeNode
默认 `serial_rs485`，避免把未验证能力当作已经可用。

## NAND 与启动兼容性

芯片为 GD5F4GM7UEYI，ID `c8 94`，容量 512 MiB。

| 层级 | 页 | 擦除块 | OOB |
| --- | --- | --- | --- |
| 芯片物理结构 | 4096 | 262144 | 128 |
| 原厂 AW 驱动逻辑结构 | 8192 | 524288 | 256 |

原厂 MTD：boot0 2 MiB、uboot 6 MiB、secure_storage 2 MiB、sys 502 MiB。
sys 中的 UBI 使用 4096 字节 VID offset、8192 字节 data offset、516096
字节 LEB；rootfs 是 ubi0_5。原厂镜像内 `rootfs-ubifs.fex` 是 UBIFS 载荷，
不是可直接刷写的完整 UBI 分区镜像。

标准主线 SPI-NAND/UBI 的物理页布局不能直接替代 AW 逻辑页布局。
必须检查芯片识别/ECC 支持、原厂页映射和坏块处理，备份并验证恢复路径，
再设计可回退的安装及升级方案。不能照搬其他 T113-S3 开发板的 UBI 参数。

可以改用主线原生的 4096 字节页 / 262144 字节擦除块。这是存储格式迁移，
不是把已写入的 NAND 修改两个驱动参数就能继续使用：UBI/UBIFS 必须按新几何
重新生成；U-Boot 加载内核的存储路径也必须与新布局一致。旧文件可以先导出、
再恢复到新文件系统，但旧的原始 UBI/UBIFS 镜像不能直接视为新布局镜像。
初始日志只确认上下两层的几何尺寸。随后从镜像的 `vmlinux.fex`（tar.bz2）
提取带 DWARF 的 ARM 内核 ELF，并对照公开 AW 驱动，确认原厂的
`super_to_phy`、`aw_spinand_info_page_size`、`aw_spinand_info_block_size` 和
`aw_spinand_chip_isbad_super_block` 实现：

- 逻辑块 B 对应物理块 2B 和 2B+1。
- 逻辑页 P 的前 4096 字节来自物理块 2B 的页 P，后 4096 字节来自
  物理块 2B+1 的页 P；不是把同一物理块的前后两页直接连接。
- 逻辑坏块检查同时检查这两个物理块，任一坏块都令逻辑块不可用。
- DWARF 中原厂请求结构大小40字节；`block/page/pageoff` 偏移为0/4/8，
  与上面的汇编访存一致。代码编译路径指向 Linux 5.4 的 AW SPI-NAND 驱动。
- DWARF 枚举确认实机 `EccType=11` 是 `BIT4_LIMIT5_TO_7_ERR8_LIMIT_12`，
  `EccProtectedType=1` 是 `SIZE16_OFF0_LEN16`。这只是识别原厂语义，
  不能直接套用另一个型号的主线 ECC 回调；还要核对状态寄存器读取和统计行为。

上述地址关系以原厂驱动使用的整个 NAND 逻辑地址为基准，不能对 sys 分区
随意重置块号或压缩坏块。当前 sys 起点10 MiB，必须在兼容层中正确保留这个
绝对偏移。启动区访问路径、OOB、坏块标记和写入约束仍须继续校对。
相关反汇编证据留在本地 `artifacts/ebyte-t113i/vendor-nand-disassembly.txt`。

公开参考来自
[AW SPI-NAND 源码](https://github.com/YuzukiHD/TinyVision/tree/main/kernel/linux-4.9/drivers/mtd/awnand/spinand)，
与本机 Linux 5.4 驱动版本不同，只作对照，不直接复制为已验证驱动。

### SDK 检索与交叉核对（2026-10-21）

用户说明 ECM30 是 OEM 产品，可以用其他 T113 核心板的 SDK 对照。
已经核查以下厂商入口；厂商公开的内核版本、资料目录与已取得的源码分别记录，
不能将产品页宣传的“提供源码”视为已经拿到对应完整 SDK。

| 来源 | 已核实的基线和获取情况 | 对本次适配的用途 |
| --- | --- | --- |
| [东山派 T113i 工业套件](https://dshanpi.100ask.org/docs/T113i-Industrial/part3/DevelopmentEnvironmentSetup/) | Tina5 SDK 基础包约3.3 GB，基础包另配 buildroot/openwrt/platform；[GitHub 仓库](https://github.com/DongshanPI/T113i_DevKitF_Tina5SDK)是板级扩展包，含 `evb1_auto_nand` 配置，并非完整内核源码 | T113-i NAND 板级配置、构建和打包流程 |
| [创龙 SOM-TLT113](https://www.tronlong.com/productinfo188.html) | T113-i，提供512 MB NAND/512 MB DDR3配置；Linux及Linux-RT 5.4.61，SDK标识 `T113_Tina5.0-V1.0`；完整开发资料入口需要申请，本次没有提交申请或取得其源码包 | 与实机容量、内核基线接近的核心板对照 |
| [米尔 MYC-YT113i](https://www.myir.cn/product/som/t113/myc-yt113i.html) | 厂商说明基于Linux 5.4.61，标准配置为eMMC；页面的软件资源还列出NAND镜像，不能据此推断当前标准核心板使用相同NAND芯片 | 启动链和外设参考；NAND料号必须另行核对 |
| [飞凌 FET113i-S](https://www.forlinx.net/product/t113i-s-system-on-module-143.mhtml) | Linux5.4.61，说明提供内核源码等开发资料；页面注明购买后提供资料，本次没有取得其完整源码包 | 另一家T113-i BSP基线对照 |

额外找到亿佰特官方产品页链接的
[T113资料盘](https://drive.google.com/drive/folders/1sMI-rRvfWIuDiLA-9C7pSEHChjIgc3xt)，
其 [02_Sources目录](https://drive.google.com/drive/folders/1QaYiP_O7PpGxU38K-r2k9D3dXuMpiJxe)
列出 [allwinner_260130.tar.gz](https://drive.google.com/file/d/19hY4ti6-l1SYfPe6BJ-GAoO8dKDw6x-j/view)，
26090452114字节（约26.1 GB / 24.3 GiB）。随后按用户要求完整下载，
SHA-256为 `40aa1a2f8f0fc17bb3238fcb0f985d6a3ec011213ec448a4089c1af642600c57`。
完整遍历264334个归档条目并验证gzip CRC，安全提取307个驱动/配置文件，
没有执行SDK中的程序或脚本。压缩包及提取清单位于本地忽略目录
`artifacts/ebyte-t113i/sdk-research/`。

SDK内核为5.4.61。已核对Linux和U-Boot的AW SPI-NAND实现：
双块映射、默认每半页16字节备用区、ECC状态组合和首字节坏块判断与镜像一致。
不过SDK的Linux/U-Boot ID表均没有实机的 `c8 94`，也没有ECM30专用配置，
因此不是用户镜像的完整可重现源码；该料号继续采用镜像ELF中已核实的参数。
SDK的 `AW_SPINAND_MTD_OPS_RAW` 在缓存路径中控制备用区拷贝，
并不等于Linux主线关闭片内ECC的 `MTD_OPS_RAW`。

已实际下载并固定版本的 Linux 5.4.61 AW NAND 参考：

- [Longan T113内核镜像仓库](https://github.com/guanglun/longan-t113-linux-5.4/tree/d51ad00c047eccd5c88e807aedf56a5a25d9a155)，
  提交 `d51ad00c047eccd5c88e807aedf56a5a25d9a155`。
- [MangoPi Tina内核的保留镜像](https://github.com/paulwratt/mangopi-sbc-tina-linux-5.4/tree/0d4903ebd9d2194ad914686d5b0fc1ddacf11a9d)，
  提交 `0d4903ebd9d2194ad914686d5b0fc1ddacf11a9d`。

两者均是公开镜像仓库，不能称为亿佰特原始源码。它们的AW NAND实现有共同来源，
也不等于两次独立的硬件验证。已取得各自的 `ops.c/id.c/ecc.c/core.c/bbt.c/cache.c`、
`sunxi-core.c` 和头文件，文件SHA-256清单保存在本地忽略目录
`artifacts/ebyte-t113i/sdk-research/`。

源码中的 `CONFIG_AW_SPINAND_SIMULATE_MULTIPLANE`、`super_to_phy()`、
`aw_spinand_chip_super_next()` 和配对坏块检查，与原厂ELF已确认的映射一致。
MangoPi版本另有 `CONFIG_AW_SPINAND_MTD_OOB_RAW_SPARE` 分支；原厂ELF
`super_to_phy()` 明确把首半页备用区请求限制为16字节，因此不能照搬该raw分支。
这两份公开版本的ID表均没有 `c8 94`；实际芯片参数继续以用户镜像为准。

从原厂ELF的 `gigadevice[]` 表和DWARF结构偏移读取到 `c8 94` 对应：
2048块、每块64页、每页8个512字节扇区、OOB128字节，
`BadBlockFlag=0`（首1页）、`OperationOpt=7`、`EccFlag=1`、
`EccType=11`、`EccProtectedType=1`。这一步只解析本地镜像，没有访问或写入板上NAND。

用户已选择保留原厂布局。后续沿用现有 boot0、U-Boot、secure_storage、
sys 分区以及 8192 字节页 / 524288 字节块的逻辑视图，不进行原生布局迁移。
实现方向是在当前 Linux 6.18 中适配原厂逻辑映射及 OOB/ECC/坏块语义，
先只读验证现有 UBI 头、卷表和文件数据，再补齐写入与升级支持。不能仅把
标准 SPI-NAND 驱动的 writesize/erasesize 改大而不实现实际映射。

保留布局可以避免重做原厂启动链和数据迁移，但不代表原厂 Linux 5.4 驱动
能直接放入 Linux 6.18 编译。用户没有提供匹配此板的SDK；本次另行下载的
亿佰特资料盘SDK仍缺ECM30及c8:94配置，继续与镜像内二进制交叉核对。
镜像中的内核二进制及调试符号用于校对，不能当作可直接移植的源码。

本目标已启用SPI0，只注册只读逻辑MTD，原生物理MTD不注册；MMC2仍禁用。
不输出NAND、factory、sysupgrade或SD卡镜像，平台升级钩子拒绝写入。
SPI-NAND核心在总线操作前拒绝WRITE ENABLE，初始化/恢复时保留硬件块保护，
兼容层没有OTP、raw或可写别名。数据/OOB读取保留ECC，坏块检查只判断
每个配对物理块第0页的第一个备用字节；不能沿用主线检查两个raw字节的默认实现。
总线错误重试后向上传递，停止验证，不把I/O故障永久登记成坏块。
固定保留boot0/uboot/secure_storage/sys边界，禁止旧bootargs中的mtdparts重划分。
UBI检测到MTD只读标志后进入ro_mode；关闭fastmap，后续挂载须显式使用只读模式。

设备树保留原厂OP-TEE占用的0x41900000–0x41ffffff，并使用原厂PSCI。
本次用户授权继续进行只读验证和所需的RAM启动，不授权刷写NAND或保存U-Boot环境。
RAM启动前还必须检查内核解压范围不会覆盖OP-TEE保留区。
压缩zImage的uImage加载/入口地址设为0x43000000，避免在0x40008000
原地解压时把压缩输入自动搬到OP-TEE保留区；解压目标仍为0x40008000。
TFTP容器和独立DTB分别可放在0x45000000、0x4f000000，启动前必须按
最终产物实际长度和内核 `_end` 再核对所有范围，不能仅检查uImage文件大小。

已从原厂系统只读采集10个sys PEB的头/尾SHA-256和UBI EC头CRC，
VID offset=4096、data offset=8192，抽样EC头CRC全部通过。
原厂MEMGETBADBLOCK确认sys相对PEB235为坏块，对应绝对逻辑块255 /
物理块510、511这一对，与启动日志中的物理坏块511一致。
原厂boot卷完整SHA-256为
`de32431458cd6f33f51d1b6c835f71be7d2a83a88ba928c9840b2abc3ebaded5`。
原厂rootfs卷的首4096字节也已记录；它与镜像载荷的首4096字节摘要不同，
不能拿离线镜像的摘要替代当前设备基线。基线仅存本地忽略目录
`artifacts/ebyte-t113i/readonly-baseline.json`，后续6.18实机对照结果见下文。

## 源码与构建

`target/linux/sunxi/config-6.18` 和 Cortex-A7 配置来自本仓库裁剪前的官方
ImmortalWrt 基线 `9716807c34c18c106fab3e8777839265111d5412`，没有引入私有 feed。
新增目标只包含 ECM30；使用仓库当前的 Linux 6.18，而非镜像中的 BSP 内核。

实验配置为 `configs/ebyte-ecm30.config`，保留 EdgeNode、LuCI、WireGuard、
HTTPS/WSS、Telnet 和 tcpdump-mini 的现有软件选择。TAS-682F / IT-694 发布
继续使用原有 `configs/tastek-mt76x8.config`，两种硬件需要分开构建。

按 AGENTS.md：本地完成源码修改、提交并推送 origin 后，才能在指定构建机
10.10.0.100（用户本次更正）的 `/home/openwrtbuild/immortalwrt-dtu` 以干净工作树
`git pull --ff-only` 更新。更新官方 feeds，运行 `scripts/install-tas-feeds.sh`，
再使用实验配置 `make defconfig` 和完整镜像构建。跨架构切换必须处理旧输出和
依赖基线，不能把原来的 MIPS 产物当作 ARM 的增量基线。

预期输出：`*-ebyte_ecm30-initramfs-uImage`、`*-ebyte_ecm30-board.dtb`
和用于解包检查的 `*-ebyte_ecm30-rootfs.tar.gz`。tar.gz不是刷机镜像；
启用该归档也为ImmortalWrt的独立DTB artifact提供正常的设备输出依赖。
镜像的封装、加载地址、原厂 U-Boot 对独立 DTB 的传递方式仍须在 RAM 启动时验证，
不能使用原厂 BSP DTB 启动主线内核。构建成功也不等于已通过启动和硬件测试。

用户随后将构建机地址更正为10.10.0.100；SSH连接现已恢复。
该主机原先没有指定发布目录，本次补齐编译依赖后将从已推送提交创建干净构建检出，
源码不会直接复制或编辑到构建机检出中。
分支为 `codex/ecm30-nand-ro`。提交 `5eb182bbd2` 中的只读驱动已在
Linux6.18.44 / x86_64 QEMU中运行9项KUnit测试，全部通过：几何尺寸、
配对页、sys绝对偏移、非对齐及跨块读取、OOB、配对坏块、ECC/短读/总线错误、
边界和模式检查、写入拒绝。此结果不替代ARM编译或实机验证。
可在干净、已提交的检出中运行 `sh scripts/test-ecm30-nand.sh` 重现；
脚本会校验官方内核压缩包SHA-256，并在build_dir内生成测试源码和QEMU内核。
完整构建之后仍须检查 manifest、最终根文件系统、ELF 依赖、密钥权限、
防火墙、镜像大小和 SHA-256，并与 seed 配置一起归档。

### 完整ARM构建和RAM加载检查

源码提交 `078d35132b`，运行完整 `make -j24 V=s` 成功，
版本 `ImmortalWrt SNAPSHOT r41207-078d35132b`，Linux `6.18.44`。
构建中补齐了UBI默认配置、NAND能力声明，以及仅RAM目标的DTB产物依赖。
原TAS发布seed没有改变。

| 产物 | 字节数 | SHA-256 |
| --- | --- | --- |
| initramfs-uImage | 9225337 | `6a5bf1df978278bf06dca5f9ce85a326862603e4d3a1e71a053014b5a0c330d6` |
| board.dtb | 19201 | `11c778dc903ef1f5ba82f27219b8f74363be41895928d9b29f2c47bff09f1be6` |
| rootfs.tar.gz | 5032341 | `e8442eeffd2cf7ff2c4641d8e40f99beffbc49a6da9b78991fd9009ec27288ee` |

已解包最终根文件系统，并从内核ELF提取、解析实际内嵌的983个cpio条目。
165个ARM ELF的动态依赖和解释器均存在；确认EdgeNode `0.3.70-r1`、
Telnet客户端/服务端、tcpdump-mini、LuCI和ubi-utils；没有嵌入私钥。
防火墙保留LAN允许、WAN输入拒绝/转发丢弃及LAN至WAN转发规则。
uImage头/数据CRC、独立与附加DTB的一致性、发布集合16个文件的SHA-256均通过。
文件、manifest、seed、展开配置、feeds/buildinfo及检查结果保存在本地忽略目录
`artifacts/ebyte-t113i/ram-078d35132b/`；Windows下集合校验文件使用
`release-sha256sums.txt`，避免与构建器的`sha256sums`大小写冲突。

最终内核 `_end` 对应物理地址 `0x41423c30`，低于OP-TEE起点`0x41900000`。
压缩输入范围`0x43000000–0x438cc438`、容器`0x45000000–0x458cc478`、
DTB `0x4f000000–0x4f004b00`也互不重叠。

实机原厂U-Boot报告`gmac get phy-clk-type failed`和`No ethernet found`。
SDK的`sunxi_gmac.c`证实缺少的是`sunxi,phy-clk-type`属性；
原厂Linux能联网，不等于此U-Boot能直接TFTP启动。原厂内核还关闭了KEXEC。
本次采用U-Boot已有的`loadx`串口RAM加载，不修改或保存原厂DTB/环境。
COM5是CH340，UART时钟24MHz；`loadx`参数可暂用1500000bps，
完成后返回115200。U-Boot的`setenv baudrate 1500000`会被其速率表拒绝，
不能用该命令替代`loadx`的临时速率参数。

停留U-Boot时会自动复位：片内WDT mode寄存器本来为0，单次设置PG3高电平
仍不能避免复位。把已确认的PG3设为GPIO输出，并在16KiB串口分段之间
脉冲喂狗后，加载过程保持稳定。GPIO操作仅用于本次RAM测试，
没有执行NAND写入、擦除或`saveenv`。此处记录的是早期加载检查；
NAND实机读取结果见下一节。

首轮RAM启动实际进入Linux6.18.44并识别ECM30和PSCI1.0，
但UART报告`clock rate not defined`，USB/加密时钟也报错。
共同SoC DTS中的`dcxo-clk`没有频率，必须由板级补充24MHz。
原厂运行时的`apb1`和`bus-uart0`也实测为24MHz；已补充`&dcxo`。
串口失联后板子自动回到原厂Ubuntu，NAND镜像未更换。
原厂U-Boot未编译`iminfo`命令，RAM加载程序改用整文件CRC和头部检查，
最终`bootm`还会校验uImage。暖复位后曾实测RAM容器和DTB仍保持原CRC，
因此后续重试先逐段检查，只重传实际不一致的分段，不能默认RAM数据仍有效。

补充晶振后的第二轮，6个UART全部注册，SPI-NAND正确识别物理512MiB /
4096字节页 / 256KiB块，并进入只读逻辑视图的四个原厂分区注册，随后在
用户空间启动前复位，没有内核panic。下一轮将chosen中的日志级别设为原厂
使用的`loglevel=3`，减少115200串口在外部看门狗1600ms窗口内的输出耗时，
继续核查GPIO看门狗的启动时序。

### Linux 6.18 NAND只读实机对照

使用 `078d35132b` 的内核和根文件系统、`7dcbe1558a` 的设备树，临时追加
`maxcpus=1 loglevel=6` 后，RAM系统稳定进入用户空间。此参数仅用于定位
双核启动复位，不是最终板级配置，也不能据此宣称双核已通过。

实机只注册4个逻辑MTD，没有物理可写别名。sys尺寸526385152字节，
页8192、块524288、OOB256字节，MTD flags=0。只读检查全部1004个PEB，
坏块仅PEB235；10个PEB的头尾摘要与原厂基线全部一致。UBI进入ro_mode=1，
识别1003个好块、1个坏块、0个损坏块和8个卷。boot卷完整SHA-256及
rootfs卷首4096字节摘要均与原厂基线一致，原厂rootfs可显式只读挂载，
读取到Ubuntu22.04.5。报告为本地
`artifacts/ebyte-t113i/ram-singlecpu-test/readonly-verification.json`。

当前UBI坏块预留默认值为20/1024，预留19块，原厂预留39块；
这是内存中的attach策略差异，本次只读挂载未改写卷或块布局。
写入、升级及全部外设功能尚未验证。

原厂运行设备树 `/timer_arch` 同时具有24MHz频率、
`arm,cpu-registers-not-fw-configured` 和 `arm,no-tick-in-suspend`。
Linux6.18的arch timer驱动在ARM32上按第一个属性选择物理定时器，
避免依赖固件初始化虚拟计数器偏移。主线SoC默认节点缺少这些板级属性，
双核启动时曾出现 `sched: DL replenish lagged too much` 后被外部看门狗复位。
板级timer节点补齐原厂属性后，双核RAM启动成功，CPU online为0-1，
日志明确为 `arch_timer: cp15 timer running at 24.00MHz (phys).`。
在同一轮293秒运行和完整NAND只读复测中未再出现上述调度异常；
这是短时验证，不代替长时间运行和电源循环测试。诊断组合及日志为
`artifacts/ebyte-t113i/ram-timerfix-ec65d1ef8b/`。

随后基于同一提交 `ec65d1ef8b` 完整构建成功，版本
`ImmortalWrt SNAPSHOT r41210-ec65d1ef8b`。这套产物的内核、根文件系统
和设备树均来自该提交，不再混用前一轮内核。

| 产物 | 字节数 | SHA-256 |
| --- | --- | --- |
| initramfs-uImage | 9226212 | `39bba623e6ccfdbfcb092f7e12d7acea1b1236d9f5db5ff8b6112ca0afd16d30` |
| board.dtb | 19340 | `e1ff9cf8d0d72d52160443bf98dd11dddf0c1dcc187642ae8415aced097b63b7` |
| rootfs.tar.gz | 5032308 | `955f6d33332e936315ae2e1ebb288ccecbdd59148e0813cd47106e492bb7f485` |

发布集合的16个文件校验、uImage CRC、附加/独立DTB一致性、OP-TEE避让、
内嵌cpio及165个ARM ELF依赖检查通过；EdgeNode仍为0.3.70，
Telnet、tcpdump-mini、LuCI、ubi-utils和原有防火墙策略均保留，没有嵌入私钥。
产物和检查结果为 `artifacts/ebyte-t113i/ram-ec65d1ef8b/`；
归档 `ecm30-release-ec65d1ef8b.tar.gz` 的SHA-256为
`5c5bb1fd1dc8d4ab209dcee2cd7f9f83d10147cbef999c64b0e13af7f2442145`。

该完整产物的RAM启动及NAND只读复测也通过，报告
`artifacts/ebyte-t113i/ram-ec65d1ef8b/readonly-verification.json`。
随后持续运行仍出现复位并回到原厂系统。连续保留UART日志后证实，
复位前procd持有watchdog句柄，ubus状态为running，timeout=30、frequency=5，
内核watchdogd线程存在；接着网络设备正常退出，再打印
`reboot: Restarting system`。这次是正常的软件重启，不是外部看门狗直接拉复位。
公共 `/usr/sbin/network-watchdog` 设有断网300秒主动重启，当前网口尚未工作，
因此会触发这一策略。不能把它与此前虚拟定时器导致的早期复位混为一谈。

给网络看门狗增加 `network_watchdog.main.reboot_on_offline` 布尔配置，
默认仍为1，保持已部署TAS固件的行为。ECM30仅RAM目标设为0，
只跳过断网重启分支；EdgeNode主进程缺失/卡死后的独立恢复、procd respawn、
硬件watchdog及内核800ms喂狗均保留。修正后还须在完整构建产物上复测。

三组隔离的策略测试执行实际网络看门狗主循环，均通过：默认断网400秒时
调用重启；禁用断网重启时不调用重启且仍检查主进程恢复；管理通道在线时
跳过公网ping且仍检查主进程恢复。两个shell文件的语法检查通过。
测试中的ping、reboot、logger等均为替身，没有执行实机或主机重启。

策略修正提交 `ebfb1ea726` 的完整ARM构建成功，版本
`ImmortalWrt SNAPSHOT r41211-ebfb1ea726`。最新RAM测试产物为
`artifacts/ebyte-t113i/ram-ebfb1ea726/`。

| 产物 | 字节数 | SHA-256 |
| --- | --- | --- |
| initramfs-uImage | 9225372 | `4f29a5e8ef84f3106ec5c1406bc922b0d08e0330f08ce7fda43fe2ba26d532ed` |
| board.dtb | 19340 | `e1ff9cf8d0d72d52160443bf98dd11dddf0c1dcc187642ae8415aced097b63b7` |
| rootfs.tar.gz | 5032759 | `ff1b33b0abe25730c36d3e994d576052c77b128b251c864f46e655bf5ba88d19` |

已解包检查最终rootfs，三个策略文件均与已提交源码一致。内嵌cpio增加为
984个条目，165个ARM ELF依赖、镜像CRC、DTB一致性、OP-TEE避让和发布集合
校验全部通过。归档 `ecm30-release-ebfb1ea726.tar.gz` 的SHA-256为
`7503a082392215b177fd9de9d6646cd504548099d6fb37e13e699cf118a95520`。
NAND驱动、设备树和内核配置与上一轮只读验证一致；这次变更仅涉及网络看门狗策略。

### 原厂auto_test补充依据

板载原厂rootfs中找到 `/root/auto_test`，共9个shell脚本，日期为2026-10-13。
已只读导出，每个文件均与板上SHA-256校验一致，保存为本地忽略目录
`artifacts/ebyte-t113i/vendor-auto-test/`。没有执行这些脚本。
当前357805056字节离线镜像的UBIFS索引中没有该目录，不能假设镜像与板上
已安装文件完全相同。脚本及其说明作为硬件参考，不能扩展本次只读授权。

| 脚本 | 原厂测试设计 | 主线适配含义 |
| --- | --- | --- |
| rs485_test.sh | ttyAS1与ttyAS3，115200、8N1，驱动自动控向 | 对应ttyS1/ttyS3；仍需核对PG13/PG2的控制语义及实测 |
| uart.sh | ttyAS2与ttyAS5双向TTL测试，115200、8N1 | 对应ttyS2/ttyS5，需要外部测试连线 |
| dual_NIC_test.sh | eth0/eth1分别移入netns，192.168.1.10/.20互ping | 是双口连线测试，不包含PHY初始化或复位配置；会改网络状态 |
| get_di.sh | GPIO物理低电平返回逻辑1 | DI按低有效解释 |
| set_do.sh | 直接写GPIO值并读回 | 仅确认控制电平；不能推断负载侧继电器极性或安全初态 |
| init.sh / finish.sh | 读取thermal_zone温度 | 没有额外GPIO、PHY或4G初始化 |
| memtester.sh | memtester 1 1并统计18条ok | 不是完整DDR容量和长时间稳定性测试 |

`di_do_65.sh` 的测试连线给出以下映射，尚未切换或驱动任何输出：

| 端子 | GPIO | 端子 | GPIO |
| --- | --- | --- | --- |
| X0 | PD16 | Y00 | PD21 |
| X1 | PE10 | Y01 | PD19 |
| X2 | PE15 | Y02 | PD18 |
| X3 | PE16 | Y03 | PD17 |
| — | — | Y10 | PB4 |
| — | — | Y11 | PB5 |
| — | — | Y12 | PB6 |

该目录没有4G测试脚本，也没有PE7 PHY复位的时序依据。主线实现需替换
BSP的debugfs路径 `/sys/kernel/debug/pinctrl/pio/`，不能照搬GPIO查找脚本。
双网口脚本、GPIO输出脚本和串口发送测试均须在正确接线、隔离现有业务后验证，
不能把文件导出或脚本中的“success”字符串当作测试已经成功。

## 4G 本次诊断

模块版本 `EC200UCNAAR03A09M08`。最初 USB、ECM、AT 正常，但实时
`AT+CPIN?` 返回 `+CME ERROR: 10`，`QSIMSTAT` 显示无卡；ModemManager
保存的 CMCC 信息是旧状态。

按用户授权执行 `AT+CFUN=1,1` 后，USB 重新枚举，整板 uptime 连续。
随后 `CPIN: READY`、`CEREG: 0,1`、`CGATT: 1`，注册中国移动 LTE Band 3。
CSQ=25，RSRP=-90 dBm，RSRQ=-11 dB，QCSQ 的 SINR 原始值55，按本型号
公式 `55/2 - 23.5` 为4 dB。信号强度可用，干扰余量一般。

2026-10-22的Linux6.18.44 RAM系统读取PDP上下文1，沿用模块现存的
`IP / CMNET`，执行临时ECM连接 `AT+QNETDEVCTL=1,1,1`。
DHCP获取IPv4、网关和两个运营商DNS，`QNETDEVCTL?`确认PDN状态1。
绑定usb0的公网IP ping 3/3成功，域名ping 2/2成功，HTTP 204请求成功。
相隔数分钟再次绑定usb0检查公网IP，2/2成功。原始日志保存在
`artifacts/ebyte-t113i/ram-ebfb1ea726/modem-*.log`，没有把SIM身份加入源码。

提交 `dc0e54681a` 为既有4ginfo监控进程增加显式启用的
`quectel_ecm_auto_connect`，默认关闭，仅ECM30板级默认开启。
每次检查先查询ECM状态；已有活动连接保持不动，断开时仅在附着后请求
上下文1的临时连接并重启 `wwan_4g` 的DHCP。沿用既有AT文件锁和procd
监控实例，不新增竞争AT端口的轮询进程，也不保存模块自动拨号策略。
板级默认增加usb0 DHCP接口、蜂窝WAN防火墙绑定和路由metric20。
按用户要求，eth0和eth1均加入br-lan，不再创建有线WAN；EdgeNode板级默认的
LAN设备为br-lan，受保护的蜂窝WAN设备为usb0。
八项PTY测试执行实际C串口事务：已连接、保留其他活动PDP、附着后拨号、
未附着等待、拒绝拨号、命令不支持、响应不完整及未知状态，全部通过。
完整镜像 `8f925c9ed1` 启动前先临时断开旧ECM连接，新系统第16秒自动请求
上下文1连接，第17秒DHCP成功；`QNETDEVCTL?` 返回1,1,1,1，绑定usb0的公网
ping 3/3成功。再次执行临时断开后等待35秒，监控自动重拨，公网ping再次3/3成功。
日志为本地 `ram-8f925c9ed1/4g-auto-connect-check.log` 和
`4g-disconnect-recovery.log`。用户选择先只验证板子联网，本次不修改电脑的
网关/DNS，也不把LAN客户端经4G转发上网表述为已经实测。

参考：[移远 EC200U AT 手册](https://quectel.com/content/uploads/2024/02/Quectel_EC200UEG91xUEG915G_Series-AT-Commands-Manual_V1.1.pdf)。

## 网口驱动与SDK对照

ECM30具有两个RJ45，但仅第一个连接T113原生EMAC，第二个通过
USB hub连接沁恒CDC Ethernet设备（1a86:5397）。原厂系统分别命名eth0、eth1。
原生MAC探测失败时USB网卡会占用eth0，不能把这个临时名称当作物理接线改变。

从同一SDK归档补充提取221个以太网、PHY及MDIO参考文件，并验证gzip CRC。
Linux5.4的 `drivers/net/ethernet/allwinner/sunxi-gmac.c` 与原厂镜像
`geth_open`、`geth_phy_init` 的反汇编吻合：RMII模式选择0x2001，外部PHY
先复位和初始化，再进行MAC软件复位；PE7拉低50ms、释放50ms。
这也排除了另一套sunxi-gmac-ng/U-Boot代码中0x2000选择值的混用。

Linux6.18采用已启用的dwmac-sun8i控制器驱动。板级把PE7复位放在MDIO总线，
保证读PHY ID前完成释放，延时匹配原厂50ms/50ms。MAC复位顺序参考上游
[net-next v6补丁](https://lore.kernel.org/r/20260927-submit-h616-emac1-v1-v6-1-e64971f4e414@gmail.com)，
移到PHY初始化后的DMA reset回调，并保留错误时关闭DMA/中断。
该补丁是6.18目标的回移，不能表述为6.18原生已合入的功能。
Generic PHY可以识别ID，但不能为本板配置RMII参考时钟方向。只进行标准BMCR
软复位、移除掉电位及匹配原厂延迟后，MAC仍复位超时。实机读取PHY的P7:R16为
0xdf5a，bit12=1表示接收50MHz；将该位清零为0xcf5a，由PHY供给50MHz后，
原生MAC复位完成，链路达到100Mbps全双工。电脑ping板子4/4成功，LuCI返回
预期的未登录响应，收发错误计数为0。

新增仅精确匹配ID 0x937c4024的 `jlsemi-ecm30.c`，在PHY初始化阶段完成上述
操作；只有ECM30设备树的 `jlsemi,rmii-refclk-out` 属性启用时才修改时钟方向。
使用标准分页MDIO辅助函数保护总线并恢复原页，保留其他寄存器位。
寄存器语义参考[HPMicro官方JLSemi驱动](https://github.com/hpmicro/hpm_sdk/blob/main/components/enet_phy/jl1111/hpm_jl1111_regs.h)，
没有据此把实机硅片型号断言为JL1111。DTS同时保留原厂TX/RX延迟档位1/1，
即各100ps；该配置对应实机已验证的syscon值0x2421。

USB网口使用CH397，CDC-ECM通用驱动虽能枚举，却未完整设置厂商接收过滤器。
当br-lan采用其他MAC时，芯片会丢弃目标不是自身硬件MAC的单播。对照
[沁恒官方V1.5驱动](https://github.com/WCHSoftGroup/ch397_linux/blob/main/driver/ch397.c)，
为精确USB ID 1a86:5397增加易失性MAC过滤寄存器更新，保留标准CDC请求。
混杂接收清除MAC_CFG的bit22/21/20，置bit19，其余位原样保留；固件版本
0x37及以上使用0x1e命令，旧版使用0x12。未访问OTP或保存配置。

实机芯片固件版本0x53，普通过滤值0x00706188；桥接混杂接收值0x00086188。
最初补丁在加载期间可能遇到桥接加入及链路重设过滤器的时序问题；修订版按
厂商延迟100ms后读取实时网络标志，并在链路建立通知后重新调度过滤更新。
独立编译正式cdc_ether模块后，在现有RAM系统重新加载，不用诊断工具写寄存器，
已自动进入混杂接收。桥接MAC依次采用02:11:22:33:44:55和02:11:22:33:44:66，
电脑ping均4/4成功；eth1关闭后重新启用也自动恢复，ping4/4、LuCI响应通过。
因此不需要把桥接MAC固定为USB网卡MAC；可设置有效的单播MAC并避免局域网重复。
日志为本地 `cdc-retest-autofilter.log`、`cdc-retest-bridge-mac.log`、
`cdc-retest-reopen.log`。完整镜像首次启动仍须复测，不能把模块热替换等同于冷启动。

网口和4G修正的完整构建提交为 `8f925c9ed1`，版本
`ImmortalWrt SNAPSHOT r41213-8f925c9ed1`。归档及全部成员SHA-256校验通过，
已解包rootfs并比对板级默认源码，内嵌cpio中165个ARM ELF依赖检查通过，
解压后的内核末端0x41423c30仍避开OP-TEE的0x41900000保留区。
产物保存在 `artifacts/ebyte-t113i/ram-8f925c9ed1/`。

| 产物 | 字节数 | SHA-256 |
| --- | --- | --- |
| initramfs-uImage | 9229389 | `dac98b8047200648b8a221fe20de0e9a192b3d9f1d5b88e3f136bbc0cec35dad` |
| board.dtb | 19341 | `3d9c1496f3fc13cf0888f447aa3af4f009367e2f7a44932aaec1026019946cdf` |
| rootfs.tar.gz | 5033272 | `7557f1212926701255a7428bd4ed8aeacd8b89ae4fce05fc96d3826ea11035e8` |

归档 `ecm30-release-8f925c9ed1.tar.gz` 的SHA-256为
`96c94c9c5228d5ead008822961687baf14d54b3b0a8eaa4f04b9c692160b0d7c`。
这是RAM验证产物，不是刷机镜像。测试前已用 `QNETDEVCTL=0` 断开旧会话，
随后已确认新镜像独立完成开机拨号及断线重连。原生PHY与CH397修正最初在这套
系统中通过易失性诊断/模块热替换验证，未包含在该提交的原始镜像内。

### 双LAN与蜂窝WAN完整产物

提交 `ca358b97a6` 完整构建通过，版本 `ImmortalWrt SNAPSHOT r41218-ca358b97a6`，
Linux6.18.44。包含PHY供给RMII参考时钟、原厂延迟档位、CH397过滤和初始链路
重设修正、eth0/eth1桥接LAN及usb0蜂窝WAN默认值。

| 产物 | 字节数 | SHA-256 |
| --- | --- | --- |
| initramfs-uImage | 9229852 | `0eaf8e782059f0cba7401b12822a038a8315bdcabf2b65085dc3344abc3dbca7` |
| board.dtb | 19452 | `0a854c5a22b2bf1642570a61992a57ecdaf2ed721affdb126bad781121468047` |
| rootfs.tar.gz | 5036062 | `77d6cd9b866b251846311235029ee0d04161ea38da928e9b72cb4e0b7229b5bb` |

产物目录为本地 `artifacts/ebyte-t113i/ram-ca358b97a6/`，包含manifest、seed、
展开配置、实际内核配置及校验清单。归档 `ecm30-release-ca358b97a6.tar.gz`
SHA-256为 `3e14b18d7a2ed5150498efb235ff9f5ce73e3e6a13662fb286bec7355968446a`。
归档及18个成员SHA-256、uImage CRC、附加/独立DTB一致性、984个cpio条目和
165个ARM ELF依赖全部检查通过；内核物理末端仍为0x41423c30，避开OP-TEE。
已确认PHY驱动编入内核、DT启用板级时钟属性、cdc_ether模块包含CH397修正，
并核对双LAN、4G及EdgeNode的实际板级默认文件。该集合仍只用于RAM测试。

该完整镜像RAM启动通过，双核online=0-1；4G开机第15秒请求ECM连接，第16秒
获得DHCP地址。绑定usb0的公网ping 3/3、域名ping 2/2、HTTP请求均通过。
原厂四个MTD分区、8192/524288/256页/块/OOB几何及flags=0只读标志保持一致。

CH397在首次启动时自动进入桥接混杂接收，br-lan默认使用原生口MAC
6a:0a:9b:92:43:8f，与USB硬件MAC不同，电脑ping4/4通过；随后通过UCI把
桥接MAC配置为02:11:22:33:44:55并reload网络。电脑最初仍缓存旧MAC，
ARP更新后ping4/4通过，不能将旧ARP缓存误判为硬件过滤失败。eth1关闭后
重新启用，过滤器自动恢复为0x00086188，ping4/4和LuCI响应通过。
诊断工具全程只读过滤寄存器，没有辅助写入。

完整镜像同时暴露原生PHY第二次打开时的 `phy_poll_reset failed: -110`：
通用软复位只清ISOLATE，保留上次关闭设置的PDOWN，而该PHY在掉电位仍置位时
不完成复位。提交 `06455bc6c9` 在本板专用复位路径先清PDOWN，再调用标准
genphy_soft_reset。该源码独立编译为临时模块并绑定到现有RAM系统的PHY，
连续四次打开返回0，没有新的复位超时。用户把线缆移回eth0后，协商100Mbps
全双工，电脑ping4/4、LuCI响应通过；br-lan仍使用上述自定义MAC。
板子经usb0访问公网保持正常。原始日志为本地 `ram-ca358b97a6/` 下的
`4g-boot-internet.log`、`4g-dns-http.log`、`usb-custom-mac-*.log`、
`usb-reopen-*.log`、`phy-retest-reopen.log` 和 `native-retest-*.log`。
PHY修正不在ca358b97a6原始镜像中，不能把这组热替换结果误写为该原始内核已无问题。
