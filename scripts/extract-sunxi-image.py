#!/usr/bin/env python3
"""Inspect/extract unencrypted IMAGEWTY 0x300 containers as data only."""

import argparse
import hashlib
import json
from pathlib import Path
import struct


def inspect_image(data):
    if len(data) < 0x60 or data[:8] != b"IMAGEWTY":
        raise ValueError("not an IMAGEWTY container")
    version = struct.unpack_from("<I", data, 8)[0]
    if version != 0x300:
        raise ValueError(f"unsupported IMAGEWTY version: {version:#x}")
    declared_size = struct.unpack_from("<Q", data, 0x18)[0]
    stride, count, table = struct.unpack_from("<III", data, 0x38)
    if declared_size != len(data):
        raise ValueError("container size does not match its header")
    if stride < 0x13c or count == 0 or count > 4096 or table < 0x60:
        raise ValueError("invalid item table")
    table_end = table + count * stride
    if table_end > len(data):
        raise ValueError("truncated item table")

    items = []
    for index in range(count):
        entry = table + index * stride
        raw_name = data[entry + 0x24:entry + 0x124].split(b"\0", 1)[0]
        name = raw_name.decode("ascii")
        if (not name or name in (".", "..")
                or any(char in name for char in '/\\:<>"|?*')
                or any(ord(char) < 32 for char in name)
                or name.endswith((".", " "))):
            raise ValueError(f"unsafe item name at index {index}")
        stored, size, offset = struct.unpack_from("<QQQ", data, entry + 0x124)
        if size > stored or offset < table_end or offset + stored > len(data):
            raise ValueError(f"invalid item extent at index {index}")
        payload = data[offset:offset + size]
        items.append({
            "index": index,
            "name": name,
            "offset": offset,
            "size": size,
            "stored_size": stored,
            # The container can contain the same name more than once.
            "extracted_name": f"{index:03d}-{name}",
            "sha256": hashlib.sha256(payload).hexdigest(),
        })
    extents = sorted((item["offset"], item["offset"] + item["stored_size"])
                     for item in items)
    if any(end > next_start for (_, end), (next_start, _)
           in zip(extents, extents[1:])):
        raise ValueError("overlapping item extents")
    return {
        "format": "IMAGEWTY",
        "version": version,
        "size": len(data),
        "sha256": hashlib.sha256(data).hexdigest(),
        "items": items,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", type=Path)
    parser.add_argument("--output", type=Path,
                        help="extract into a new directory; otherwise print the index")
    args = parser.parse_args()
    data = args.image.read_bytes()
    try:
        manifest = inspect_image(data)
    except (ValueError, UnicodeError, struct.error) as exc:
        parser.error(str(exc))
    manifest["image_name"] = args.image.name
    encoded = json.dumps(manifest, indent=2, ensure_ascii=True) + "\n"
    if args.output:
        # Never overwrite an earlier analysis or follow paths from the image.
        args.output.mkdir(parents=True, exist_ok=False)
        for item in manifest["items"]:
            target = args.output / item["extracted_name"]
            with target.open("xb") as stream:
                offset, size = item["offset"], item["size"]
                stream.write(data[offset:offset + size])
        (args.output / "image-index.json").write_text(encoded, encoding="utf-8")
        print(f"Extracted {len(manifest['items'])} items into {args.output}")
        print(f"SHA-256: {manifest['sha256']}")
    else:
        print(encoded, end="")


if __name__ == "__main__":
    main()
