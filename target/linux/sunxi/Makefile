# SPDX-License-Identifier: GPL-2.0-only
# Based on the official ImmortalWrt sunxi target.

include $(TOPDIR)/rules.mk

ARCH:=arm
BOARD:=sunxi
BOARDNAME:=Ebyte T113-I ECM30
FEATURES:=gpio nand ramdisk rtc usb squashfs
SUBTARGETS:=cortexa7

KERNEL_PATCHVER:=6.12
KERNELNAME:=zImage dtbs

include $(INCLUDE_DIR)/target.mk

DEFAULT_PACKAGES += kmod-gpio-button-hotplug

define Target/Description
  NAND firmware and RAM recovery for the Ebyte T113-I ECM30.
  Retains the vendor's paired-page 8 KiB / 512 KiB NAND layout.
endef

$(eval $(call BuildTarget))
