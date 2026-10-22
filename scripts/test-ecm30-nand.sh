#!/bin/sh
# Build the committed compatibility driver in a hardware-free Linux KUnit guest.
set -eu

TOP=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$TOP"
git diff --quiet
git diff --cached --quiet

KERNEL_SERIES=$(sed -n 's/^KERNEL_PATCHVER:=//p' target/linux/sunxi/Makefile)
KERNEL_VERSIONS="target/linux/generic/kernel-$KERNEL_SERIES"
VERSION="$KERNEL_SERIES$(awk -v key="LINUX_VERSION-$KERNEL_SERIES" '$1 == key { print $3 }' "$KERNEL_VERSIONS")"
SHA256=$(awk -v key="LINUX_KERNEL_HASH-$VERSION" '$1 == key { print $3 }' "$KERNEL_VERSIONS")
[ -n "$SHA256" ] || { echo "Missing kernel checksum for $VERSION" >&2; exit 1; }
REV=$(git rev-parse --short=12 HEAD)
ARCHIVE="$TOP/dl/linux-$VERSION.tar.xz"
SOURCE="$TOP/build_dir/ecm30-kunit-$REV/linux-$VERSION"
OUTPUT="$TOP/build_dir/ecm30-kunit-$REV/output"
mkdir -p "$TOP/dl" "$(dirname "$SOURCE")"
if [ ! -f "$ARCHIVE" ]; then
	wget -c -O "$ARCHIVE.part" "https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-$VERSION.tar.xz"
	printf '%s  %s\n' "$SHA256" "$ARCHIVE.part" | sha256sum -c -
	mv "$ARCHIVE.part" "$ARCHIVE"
fi
printf '%s  %s\n' "$SHA256" "$ARCHIVE" | sha256sum -c -
if [ ! -d "$SOURCE" ]; then
	mkdir "$SOURCE"
	tar -xJf "$ARCHIVE" -C "$SOURCE" --strip-components=1
	patch -d "$SOURCE" -p1 < "target/linux/sunxi/patches-$KERNEL_SERIES/300-mtd-spinand-ecm30-aw-view.patch"
	cp target/linux/sunxi/files/drivers/mtd/nand/spi/aw-compat*.c "$SOURCE/drivers/mtd/nand/spi/"
	cp target/linux/sunxi/files/drivers/mtd/nand/spi/aw-compat.h "$SOURCE/drivers/mtd/nand/spi/"
	touch "$SOURCE/.ecm30-patched"
fi
[ -f "$SOURCE/.ecm30-patched" ] || { echo 'Incomplete KUnit source preparation' >&2; exit 1; }
cd "$SOURCE"
python3 tools/testing/kunit/kunit.py run --arch=x86_64 \
	--kunitconfig="$TOP/scripts/ecm30-nand.kunitconfig" \
	--build_dir="$OUTPUT" --jobs="${JOBS:-24}" --timeout=120
