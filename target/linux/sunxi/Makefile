# SPDX-License-Identifier: GPL-2.0-only
# Based on the official ImmortalWrt sunxi target.

include $(TOPDIR)/rules.mk

ARCH:=arm
BOARD:=sunxi
BOARDNAME:=Ebyte T113-I ECM30 (RAM bring-up)
FEATURES:=gpio nand ramdisk rtc usb
SUBTARGETS:=cortexa7

KERNEL_PATCHVER:=6.12
KERNELNAME:=zImage dtbs

include $(INCLUDE_DIR)/target.mk

DEFAULT_PACKAGES += kmod-gpio-button-hotplug

define Target/Description
  Experimental RAM-only firmware for the Ebyte T113-I ECM30.
  Includes a read-only compatibility view of the vendor paired-block NAND.
endef

$(eval $(call BuildTarget))
