#!/bin/sh
# Legacy checked entry point. New installations use LuCI/sysupgrade directly.
# Usage: sh ecm30-install-nand.sh --check|--install IMAGE IMAGE_SHA256 BOOT_SHA256
# OpenWrt's shared shell helpers intentionally read unset optional variables.
set -e

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

[ "$(sha256sum "$image" | cut -d ' ' -f 1)" = "$image_sha" ]
platform_check_image "$image"
ecm30_check_factory_layout
[ "$(sha256sum /dev/ubi0_4 | cut -d ' ' -f 1)" = "$boot_sha" ]
echo "ECM30_INSTALL_CHECK_OK: boot 33 LEBs, rootfs 64 LEBs, rootfs_data uses remaining space"
[ "$mode" = --install ] || exit 0
exec sysupgrade -n "$image"
