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
# 16 physical 512 KiB eraseblocks, each with 516096 bytes available to UBI.
ECM30_ROOT_BYTES=8257536

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

ecm30_check_nand() {
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
	[ "$(cat /sys/class/ubi/ubi0_4/reserved_ebs)" = 33 ] || return 1
	[ "$(cat /sys/class/ubi/ubi0_4/usable_eb_size)" = 516096 ] || return 1
	[ "$(cat /sys/class/ubi/ubi0_5/usable_eb_size)" = 516096 ] || return 1
	for id in 4 5; do
		[ "$(cat /sys/class/ubi/ubi0_$id/upd_marker)" = 0 ] || return 1
		[ "$(cat /sys/class/ubi/ubi0_$id/corrupted)" = 0 ] || return 1
	done
	for name in boot0 uboot secure_storage; do
		id="$(find_mtd_index "$name")"
		[ -n "$id" ] || return 1
		[ $(( $(cat /sys/class/mtd/mtd$id/flags) & 1024 )) -eq 0 ] || return 1
	done
}

ecm30_check_layout() {
	ecm30_check_nand || return 1
	[ "$(nand_find_volume ubi0 rootfs_data)" = ubi0_8 ] || return 1
	[ "$(cat /sys/class/ubi/ubi0_5/reserved_ebs)" = 16 ] || return 1
	[ "$(cat /sys/class/ubi/ubi0_8/upd_marker)" = 0 ] || return 1
	[ "$(cat /sys/class/ubi/ubi0_8/corrupted)" = 0 ] || return 1
}

ecm30_check_install_layout() {
	local entry id name lebs
	# Initial installation and shrinking earlier 16/32 MiB allocations both
	# require the initramfs recovery system, with the old volumes unmounted.
	[ "$(rootfs_type)" = tmpfs ] || return 1
	ecm30_check_nand || return 1
	[ "$(get_magic_long /dev/ubi0_4)" = 414e4452 ] || return 1
	case "$(cat /sys/class/ubi/ubi0_5/reserved_ebs)" in
	920)
		[ "$(cat /sys/class/ubi/ubi0/volumes_count)" = 8 ] || return 1
		[ -z "$(nand_find_volume ubi0 rootfs_data)" ] || return 1
		;;
	32|64)
		[ "$(cat /sys/class/ubi/ubi0/volumes_count)" = 9 ] || return 1
		[ "$(nand_find_volume ubi0 rootfs_data)" = ubi0_8 ] || return 1
		[ "$(cat /sys/class/ubi/ubi0_8/upd_marker)" = 0 ] || return 1
		[ "$(cat /sys/class/ubi/ubi0_8/corrupted)" = 0 ] || return 1
		;;
	*) return 1 ;;
	esac
	# Refuse unknown layouts and preserve all non-system volumes.
	for entry in 0:mbr:1 1:boot-resource:1 2:env:1 3:env-redund:1 \
		6:private:2 7:UDISK:1; do
		id="${entry%%:*}"
		entry="${entry#*:}"
		name="${entry%%:*}"
		lebs="${entry##*:}"
		[ "$(nand_find_volume ubi0 "$name")" = "ubi0_$id" ] || return 1
		[ "$(cat /sys/class/ubi/ubi0_$id/reserved_ebs)" = "$lebs" ] || return 1
		[ "$(cat /sys/class/ubi/ubi0_$id/upd_marker)" = 0 ] || return 1
		[ "$(cat /sys/class/ubi/ubi0_$id/corrupted)" = 0 ] || return 1
	done
	! grep -Eq '(^|[ /])(ubi0:rootfs(_data)?|ubi0_[58]|ubiblock0_[58])([[:space:]]|$)' /proc/mounts
}

platform_check_image() {
	ecm30_check_tar "$1" || {
		echo "Expected an ECM30 sysupgrade archive with Android kernel and SquashFS rootfs." >&2
		return 1
	}
	ecm30_check_layout || ecm30_check_install_layout || {
		echo "Unsupported ECM30 NAND layout; first installation requires the RAM recovery system." >&2
		return 1
	}
}

# Called by the standard nand_upgrade_tar(). These factory boot allocations
# remain fixed; a failed attach must never trigger a whole-device ubiformat.
nand_upgrade_prepare_ubi() {
	[ "$2" = squashfs ] || return 1
	[ "$1" -gt 0 ] && [ "$1" -le "$ECM30_ROOT_BYTES" ] || return 1
	[ "$3" -gt 0 ] && [ "$3" -le "$ECM30_BOOT_BYTES" ] || return 1
	if ! ecm30_check_layout; then
		ecm30_check_install_layout || return 1
		# Like other ARM NAND targets, migrate during sysupgrade, after image
		# validation and service shutdown. Never reformat the enclosing UBI.
		nand_remove_ubiblock ubi0_5 || return 1
		ubirmvol /dev/ubi0 -N rootfs || return 1
		if [ -n "$(nand_find_volume ubi0 rootfs_data)" ]; then
			ubirmvol /dev/ubi0 -N rootfs_data || return 1
		fi
		ubimkvol /dev/ubi0 -n 5 -N rootfs -s "$ECM30_ROOT_BYTES" || return 1
		ubimkvol /dev/ubi0 -n 8 -N rootfs_data -m || return 1
		ecm30_check_layout || return 1
	fi
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

ecm30_reclaim_empty_volumes() {
	local name volume expected actual
	for name in private UDISK; do
		volume="$(nand_find_volume ubi0 "$name")"
		# SHA-256 of the complete original volume filled with 0xff. The
		# digest also fixes its length; never discard data on another board.
		case "$name:$volume" in
		private:ubi0_6)
			expected=1d8e12e00c250138c0b81892d247492212001046c1d95cddf4563a61125181af
			;;
		UDISK:ubi0_7)
			expected=132ed1c2328a15af97bc16de142a8f703d5c5f8263cfcb2e2eb979ff9db53fc9
			;;
		*) continue ;;
		esac
		[ "$(cat /sys/class/ubi/$volume/upd_marker)" = 0 ] || continue
		[ "$(cat /sys/class/ubi/$volume/corrupted)" = 0 ] || continue
		actual="$(sha256sum "/dev/$volume")"
		[ "${actual%% *}" = "$expected" ] || continue
		echo "Reclaiming empty $name volume for rootfs_data"
		ubirmvol /dev/ubi0 -N "$name" || return 1
	done
	return 0
}

platform_do_upgrade() {
	local file="$1"
	platform_check_image "$file" || return 1
	# stage2 has switched to RAM; use OpenWrt's common UBI tar writer.
	nand_upgrade_tar "$file" || return 1
	ecm30_verify_volume "$file" "$ECM30_TAR_DIR/root" ubi0_5 || return 1
	ecm30_verify_volume "$file" "$ECM30_TAR_DIR/kernel" ubi0_4 || return 1
	ecm30_reclaim_empty_volumes || return 1
	# Rebuild the overlay and restore only the sysupgrade configuration backup,
	# avoiding stale overlay binaries, as in the standard NAND updater.
	ubirmvol /dev/ubi0 -N rootfs_data || return 1
	ubimkvol /dev/ubi0 -n 8 -N rootfs_data -m || return 1
	nand_do_upgrade_success
}
