#!/bin/sh
# Initial migration from the backed-up factory layout, from a RAM recovery boot.
# Usage: sh ecm30-install-nand.sh --check|--install IMAGE IMAGE_SHA256 BOOT_SHA256
set -eu

[ "$#" = 4 ] || { echo "Expected mode, image, image SHA-256, original boot SHA-256" >&2; exit 2; }
mode="$1" image="$2" image_sha="$3" boot_sha="$4"
case "$mode" in --check|--install) ;; *) exit 2 ;; esac
for digest in "$image_sha" "$boot_sha"; do
	[ "${#digest}" = 64 ] || exit 2
	case "$digest" in *[!0-9a-f]*) exit 2 ;; esac
done

. /lib/functions.sh
. /lib/functions/system.sh
. /lib/upgrade/common.sh
. /lib/upgrade/nand.sh
. /lib/upgrade/platform.sh

[ "$(board_name)" = "ebyte,ecm30" ]
grep -q '^tmpfs / tmpfs ' /proc/mounts
grep -q '^tmpfs /tmp tmpfs ' /proc/mounts
[ "$(sha256sum "$image" | cut -d ' ' -f 1)" = "$image_sha" ]
ecm30_check_tar "$image"

mtd="$(find_mtd_index sys)"
case "$mtd" in ''|*[!0-9]*) exit 1 ;; esac
[ "$(cat /sys/class/mtd/mtd$mtd/size)" = 526385152 ]
[ "$(cat /sys/class/mtd/mtd$mtd/writesize)" = 8192 ]
[ "$(cat /sys/class/mtd/mtd$mtd/erasesize)" = 524288 ]
[ $(( $(cat /sys/class/mtd/mtd$mtd/flags) & 1024 )) -ne 0 ]
[ "$(nand_find_ubi sys)" = ubi0 ]
[ "$(cat /sys/class/ubi/ubi0/ro_mode)" = 0 ]
[ "$(nand_find_volume ubi0 boot)" = ubi0_4 ]
[ "$(nand_find_volume ubi0 rootfs)" = ubi0_5 ]
[ -z "$(nand_find_volume ubi0 rootfs_data)" ]
[ "$(cat /sys/class/ubi/ubi0_4/reserved_ebs)" = 33 ]
[ "$(cat /sys/class/ubi/ubi0_5/reserved_ebs)" = 920 ]
[ "$(cat /sys/class/ubi/ubi0_5/usable_eb_size)" = 516096 ]
[ "$(sha256sum /dev/ubi0_4 | cut -d ' ' -f 1)" = "$boot_sha" ]
for id in 4 5; do
	[ "$(cat /sys/class/ubi/ubi0_$id/upd_marker)" = 0 ]
	[ "$(cat /sys/class/ubi/ubi0_$id/corrupted)" = 0 ]
done
for name in boot0 uboot secure_storage; do
	id="$(find_mtd_index "$name")"
	[ -n "$id" ]
	[ $(( $(cat /sys/class/mtd/mtd$id/flags) & 1024 )) -eq 0 ]
done
if grep -Eq 'ubi0_5|ubiblock0_5' /proc/mounts; then
	echo "The factory rootfs must be unmounted" >&2
	exit 1
fi
echo "ECM30_INSTALL_CHECK_OK: boot 33 LEBs, rootfs 64 LEBs, rootfs_data uses remaining space"
[ "$mode" = --install ] || exit 0

# The full original boot/rootfs and small volumes must already be backed up.
# Do not format sys or alter bootloader, environment, private or UDISK volumes.
ubirmvol /dev/ubi0 -N rootfs
ubimkvol /dev/ubi0 -n 5 -N rootfs -s "$ECM30_ROOT_BYTES"
ubimkvol /dev/ubi0 -n 8 -N rootfs_data -m
ecm30_check_layout
nand_upgrade_tar "$image"
ecm30_verify_volume "$image" "$ECM30_TAR_DIR/root" ubi0_5
ecm30_verify_volume "$image" "$ECM30_TAR_DIR/kernel" ubi0_4
sync
echo "ECM30_INSTALL_READBACK_OK: ready to reboot from NAND"
