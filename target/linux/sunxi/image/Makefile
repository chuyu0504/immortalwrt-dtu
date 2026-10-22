# SPDX-License-Identifier: GPL-2.0-only

include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/image.mk

define Build/ecm30-android
	rm -f $@.android $@.json
	$(STAGING_DIR_HOST)/bin/python3 $(TOPDIR)/scripts/mk-ecm30-bootimg.py \
		--uimage $@ --dtb $(KDIR)/image-$(firstword $(DEVICE_DTS)).dtb \
		--rootfs squashfs --output $@.android --report $@.json
	mv $@.android $@
endef

define Build/ecm30-ram-dtb
	$(LINUX_DIR)/scripts/dtc/dtc -I dtb -O dts -o $@.dts $(KDIR)/image-$(firstword $(DEVICE_DTS)).dtb
	sed -i 's#bootargs = .*#bootargs = "console=ttyS0,115200 earlycon=uart8250,mmio32,0x02500000 ubi.mtd=sys,4096 loglevel=3";#' $@.dts
	$(LINUX_DIR)/scripts/dtc/dtc -I dts -O dtb -o $@.dtb $@.dts
	cat $@.dtb >> $@
	rm $@.dtb $@.dts
endef

define Device/Default
  PROFILES := Default
  KERNEL_NAME := zImage
  # Keep the compressed kernel above OP-TEE; zImage expands at 0x40008000.
  KERNEL_LOADADDR := 0x43000000
  KERNEL := kernel-bin | append-dtb | uImage none | ecm30-android
  KERNEL_SIZE := 16632k
  KERNEL_INITRAMFS := kernel-bin | ecm30-ram-dtb | uImage none
  KERNEL_INITRAMFS_SUFFIX := -uImage
  DEVICE_DTS_DIR := ../dts
  KERNEL_SUFFIX := -boot.img
  KERNEL_INSTALL := 1
  FILESYSTEMS := squashfs
  IMAGES := sysupgrade.bin
  IMAGE/sysupgrade.bin := sysupgrade-tar | append-metadata
endef

define Device/ebyte_ecm30
  DEVICE_VENDOR := Ebyte
  DEVICE_MODEL := T113-I ECM30
  DEVICE_VARIANT := NAND
  DEVICE_DTS := sun8i-t113i-ebyte-ecm30
  SUPPORTED_DEVICES := ebyte,ecm30
  ARTIFACTS := board.dtb
  ARTIFACT/board.dtb := append-dtb
  DEVICE_PACKAGES := kmod-usb2 kmod-usb-ohci kmod-usb-net-cdc-ether \
    kmod-usb-serial-option
endef
TARGET_DEVICES += ebyte_ecm30

$(eval $(call BuildImage))
