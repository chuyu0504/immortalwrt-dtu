# SPDX-License-Identifier: GPL-2.0-only

include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/image.mk

define Device/Default
  PROFILES := Default
  KERNEL_NAME := zImage
  # Keep the compressed kernel above OP-TEE; zImage expands at 0x40008000.
  KERNEL_LOADADDR := 0x43000000
  KERNEL := kernel-bin | append-dtb | uImage none
  KERNEL_INITRAMFS := kernel-bin | append-dtb | uImage none
  KERNEL_INITRAMFS_SUFFIX := -uImage
  DEVICE_DTS_DIR := ../dts
  # RAM boot only: do not emit sdcard, factory, UBI or sysupgrade images.
  IMAGES :=
endef

define Device/ebyte_ecm30
  DEVICE_VENDOR := Ebyte
  DEVICE_MODEL := T113-I ECM30
  DEVICE_VARIANT := experimental RAM bring-up
  DEVICE_DTS := sun8i-t113i-ebyte-ecm30
  SUPPORTED_DEVICES := ebyte,ecm30
  ARTIFACTS := board.dtb
  ARTIFACT/board.dtb := append-dtb
  DEVICE_PACKAGES := kmod-usb2 kmod-usb-ohci kmod-usb-net-cdc-ether \
    kmod-usb-serial-option
endef
TARGET_DEVICES += ebyte_ecm30

$(eval $(call BuildImage))
