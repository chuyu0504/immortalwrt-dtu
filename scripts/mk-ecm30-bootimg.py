#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Package a verified ECM30 initramfs uImage as an Android v2 boot image.

This only creates local files. It never opens a device or flashes firmware.
The boot volume is 33 UBI LEBs of 516096 bytes on the tested factory layout.
"""

import argparse
import hashlib
import json
from pathlib import Path
import struct
import zlib


PAGE_SIZE = 2048
KERNEL_ADDRESS = 0x43000000
DTB_ADDRESS = 0x4F000000
BOOT_VOLUME_BYTES = 33 * 516096
CMDLINE = ("console=ttyS0,115200 earlycon=uart8250,mmio32,0x02500000 "
           "rdinit=/init loglevel=3")
ANDROID_V2 = struct.Struct("<8s10I16s512s32s1024sIQIIQ")


def require(condition, message):
    if not condition:
        raise ValueError(message)


def padded(data):
    return data + bytes((-len(data)) % PAGE_SIZE)


def unpack_uimage(data, dtb):
    require(len(data) >= 64, "truncated uImage")
    header = struct.unpack(">7I4B32s", data[:64])
    require(header[0] == 0x27051956, "not a legacy uImage")
    require(zlib.crc32(data[:4] + bytes(4) + data[8:64]) == header[1],
            "uImage header CRC mismatch")
    require(header[3] == len(data) - 64, "uImage data length mismatch")
    payload = data[64:]
    require(zlib.crc32(payload) == header[6], "uImage data CRC mismatch")
    require(header[4:6] == (KERNEL_ADDRESS, KERNEL_ADDRESS),
            "unexpected kernel load/entry address")
    require(header[7:11] == (5, 2, 2, 0), "expected Linux ARM kernel, no outer compression")
    require(len(dtb) >= 40 and dtb[:4] == b"\xd0\x0d\xfe\xed", "invalid DTB magic")
    require(struct.unpack_from(">I", dtb, 4)[0] == len(dtb), "invalid DTB size")
    require(b"ebyte,ecm30\0" in dtb, "DTB is not for ECM30")
    require(payload.endswith(dtb), "uImage appended DTB does not match supplied DTB")
    kernel = payload[:-len(dtb)]
    require(len(kernel) >= 0x30, "truncated ARM zImage")
    magic, start, end = struct.unpack_from("<3I", kernel, 0x24)
    require((magic, start, end) == (0x016F2818, 0, len(kernel)),
            "ARM zImage extent does not match the kernel payload")
    return kernel


def build_image(uimage, dtb):
    kernel = unpack_uimage(uimage, dtb)
    digest = hashlib.sha1()
    # Android v2 hashes kernel, ramdisk, second, recovery_dtbo and DTB in order.
    for component in (kernel, b"", b"", b"", dtb):
        digest.update(component)
        digest.update(struct.pack("<I", len(component)))
    header = ANDROID_V2.pack(
        b"ANDROID!", len(kernel), KERNEL_ADDRESS, 0, 0, 0, 0,
        0x40000100, PAGE_SIZE, 2, 0,
        b"sun8i_arm", CMDLINE.encode(), digest.digest().ljust(32, b"\0"), b"",
        0, 0, ANDROID_V2.size, len(dtb), DTB_ADDRESS)
    image = padded(header) + padded(kernel) + padded(dtb)
    require(len(image) <= BOOT_VOLUME_BYTES, "image exceeds the factory boot volume")
    report = {
        "format": "Android boot image v2",
        "header_bytes": ANDROID_V2.size,
        "page_bytes": PAGE_SIZE,
        "image_bytes": len(image),
        "image_sha256": hashlib.sha256(image).hexdigest(),
        "input_uimage_sha256": hashlib.sha256(uimage).hexdigest(),
        "kernel_bytes": len(kernel),
        "kernel_sha256": hashlib.sha256(kernel).hexdigest(),
        "kernel_address": hex(KERNEL_ADDRESS),
        "dtb_bytes": len(dtb),
        "dtb_sha256": hashlib.sha256(dtb).hexdigest(),
        "dtb_file_offset": PAGE_SIZE + len(padded(kernel)),
        "dtb_address": hex(DTB_ADDRESS),
        "cmdline": CMDLINE,
        "factory_boot_volume_bytes": BOOT_VOLUME_BYTES,
        "rootfs": "builtin initramfs; configuration is volatile",
        "flash_performed": False,
    }
    return image, report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--uimage", required=True, type=Path)
    parser.add_argument("--dtb", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--report", required=True, type=Path)
    args = parser.parse_args()
    require(not args.output.exists() and not args.report.exists(),
            "refusing to overwrite an existing output or report")
    image, report = build_image(args.uimage.read_bytes(), args.dtb.read_bytes())
    with args.output.open("xb") as stream:
        stream.write(image)
    with args.report.open("x", encoding="utf-8") as stream:
        json.dump(report, stream, indent=2)
        stream.write("\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
