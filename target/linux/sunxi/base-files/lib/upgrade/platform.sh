# ECM30 currently supports RAM bring-up only. In particular, the stock
# Allwinner paired-page NAND layout must never enter a generic write path.
platform_check_image() {
	echo "ECM30 NAND installation is not supported; use the RAM bring-up image." >&2
	return 1
}

platform_do_upgrade() {
	echo "ECM30 flash writes are disabled during read-only NAND validation." >&2
	return 1
}
