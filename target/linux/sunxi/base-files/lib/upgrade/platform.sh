# SPDX-License-Identifier: GPL-2.0-only
# Keep the factory UBI volume IDs and never fall back to formatting sys.
REQUIRE_IMAGE_METADATA=1
RAMFS_COPY_BIN="sha256sum head"
CI_UBIPART=sys
CI_KERNPART=boot
CI_ROOTPART=rootfs
CI_SKIP_KERNEL_MTD=1

ECM30_TAR_DIR=sysupgrade-ebyte_ecm30
ECM30_BOOT_BYTES=17031168
ECM30_ROOT_BYTES=33030144

ecm30_check_tar() {
	local file="$1" listing kernel_size root_size
	[ "$(board_name)" = "ebyte,ecm30" ] || return 1
	listing="$(tar tf "$file" 2>/dev/null)" || return 1
	[ "$(printf '%s\n' "$listing" | grep -c "^$ECM30_TAR_DIR/CONTROL$")" = 1 ] || return 1
	[ "$(printf '%s\n' "$listing" | grep -c "^$ECM30_TAR_DIR/kernel$")" = 1 ] || return 1
	[ "$(printf '%s\n' "$listing" | grep -c "^$ECM30_TAR_DIR/root$")" = 1 ] || return 1
	if printf '%s\n' "$listing" | grep -Ev "^$ECM30_TAR_DIR/(CONTROL|kernel|root)?$" >/dev/null; then
		return 1
	fi
	# Read all members, including the tar checksum checks, before any mutation.
	tar xOf "$file" >/dev/null 2>&1 || return 1
	kernel_size="$(tar xOf "$file" "$ECM30_TAR_DIR/kernel" | wc -c)"
	root_size="$(tar xOf "$file" "$ECM30_TAR_DIR/root" | wc -c)"
	[ "$kernel_size" -gt 2048 ] && [ "$kernel_size" -le "$ECM30_BOOT_BYTES" ] || return 1
	[ "$root_size" -gt 96 ] && [ "$root_size" -le "$ECM30_ROOT_BYTES" ] || return 1
	[ "$(get_magic_long_tar "$file" cat "$ECM30_TAR_DIR/kernel")" = 414e4452 ] || return 1
	[ "$(get_magic_long_tar "$file" cat "$ECM30_TAR_DIR/root")" = 68737173 ] || return 1
}

ecm30_check_layout() {
	local mtd ubi name id
	mtd="$(find_mtd_index sys)"
	case "$mtd" in ''|*[!0-9]*) return 1 ;; esac
	[ "$(cat /sys/class/mtd/mtd$mtd/size)" = 526385152 ] || return 1
	[ "$(cat /sys/class/mtd/mtd$mtd/writesize)" = 8192 ] || return 1
	[ "$(cat /sys/class/mtd/mtd$mtd/erasesize)" = 524288 ] || return 1
	[ $(( $(cat /sys/class/mtd/mtd$mtd/flags) & 1024 )) -ne 0 ] || return 1
	ubi="$(nand_find_ubi sys)"
	[ "$ubi" = ubi0 ] || return 1
	[ "$(cat /sys/class/ubi/$ubi/ro_mode)" = 0 ] || return 1
	[ "$(nand_find_volume "$ubi" boot)" = ubi0_4 ] || return 1
	[ "$(nand_find_volume "$ubi" rootfs)" = ubi0_5 ] || return 1
	[ "$(nand_find_volume "$ubi" rootfs_data)" = ubi0_8 ] || return 1
	[ "$(cat /sys/class/ubi/ubi0_4/reserved_ebs)" = 33 ] || return 1
	[ "$(cat /sys/class/ubi/ubi0_5/reserved_ebs)" = 64 ] || return 1
	[ "$(cat /sys/class/ubi/ubi0_4/usable_eb_size)" = 516096 ] || return 1
	[ "$(cat /sys/class/ubi/ubi0_5/usable_eb_size)" = 516096 ] || return 1
	for id in 4 5 8; do
		[ "$(cat /sys/class/ubi/ubi0_$id/upd_marker)" = 0 ] || return 1
		[ "$(cat /sys/class/ubi/ubi0_$id/corrupted)" = 0 ] || return 1
	done
	for name in boot0 uboot secure_storage; do
		id="$(find_mtd_index "$name")"
		[ -n "$id" ] || return 1
		[ $(( $(cat /sys/class/mtd/mtd$id/flags) & 1024 )) -eq 0 ] || return 1
	done
}

platform_check_image() {
	ecm30_check_tar "$1" || {
		echo "Expected an ECM30 sysupgrade archive with Android kernel and SquashFS rootfs." >&2
		return 1
	}
	ecm30_check_layout || {
		echo "ECM30 factory-compatible writable NAND layout is not ready for sysupgrade." >&2
		return 1
	}
}

# Called by the standard nand_upgrade_tar(). These factory boot allocations
# remain fixed; a failed attach must never trigger a whole-device ubiformat.
nand_upgrade_prepare_ubi() {
	[ "$2" = squashfs ] || return 1
	[ "$1" -gt 0 ] && [ "$1" -le "$ECM30_ROOT_BYTES" ] || return 1
	[ "$3" -gt 0 ] && [ "$3" -le "$ECM30_BOOT_BYTES" ] || return 1
	ecm30_check_layout || return 1
	nand_remove_ubiblock ubi0_5
}

ecm30_verify_volume() {
	local file="$1" member="$2" volume="$3" bytes expected actual
	bytes="$(tar xOf "$file" "$member" | wc -c)"
	expected="$(tar xOf "$file" "$member" | sha256sum)"
	expected="${expected%% *}"
	[ "$(cat /sys/class/ubi/$volume/upd_marker)" = 0 ] || return 1
	[ "$(cat /sys/class/ubi/$volume/corrupted)" = 0 ] || return 1
	actual="$(head -c "$bytes" "/dev/$volume" | sha256sum)"
	[ "${actual%% *}" = "$expected" ] || return 1
}

platform_do_upgrade() {
	local file="$1"
	platform_check_image "$file" || return 1
	# stage2 has switched to RAM; use OpenWrt's common UBI tar writer.
	nand_upgrade_tar "$file" || return 1
	ecm30_verify_volume "$file" "$ECM30_TAR_DIR/root" ubi0_5 || return 1
	ecm30_verify_volume "$file" "$ECM30_TAR_DIR/kernel" ubi0_4 || return 1
	# Rebuild the overlay and restore only the sysupgrade configuration backup,
	# avoiding stale overlay binaries, as in the standard NAND updater.
	ubirmvol /dev/ubi0 -N rootfs_data || return 1
	ubimkvol /dev/ubi0 -n 8 -N rootfs_data -m || return 1
	nand_do_upgrade_success
}
