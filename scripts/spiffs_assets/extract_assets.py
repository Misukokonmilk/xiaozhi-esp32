#!/usr/bin/env python3
"""
Extract files from assets.bin (SpiffsGenerator format from xiaozhi-assets-generator)
"""

import struct
import os


def extract_assets(assets_bin_path, output_dir=None):
    if output_dir is None:
        output_dir = os.path.dirname(assets_bin_path) or "."

    os.makedirs(output_dir, exist_ok=True)

    with open(assets_bin_path, "rb") as f:
        data = f.read()

    pos = 0

    # Header: 12 bytes total
    total_files = struct.unpack("<I", data[pos : pos + 4])[0]
    pos += 4
    checksum = struct.unpack("<I", data[pos : pos + 4])[0]
    pos += 4
    combined_len = struct.unpack("<I", data[pos : pos + 4])[0]
    pos += 4

    print(f"Total files: {total_files}")
    print(f"Checksum: 0x{checksum:08X}")
    print(f"Combined data length: {combined_len}")
    print()

    # Entry size: 32 (name) + 4 (size) + 4 (offset) + 2 (width) + 2 (height) = 44 bytes
    entry_size = 44
    entries = []

    for i in range(total_files):
        entry = data[pos : pos + entry_size]
        pos += entry_size

        # Name: 32 bytes, null-padded
        name_bytes = entry[:32]
        null_pos = name_bytes.find(b"\x00")
        if null_pos >= 0:
            name = name_bytes[:null_pos].decode("utf-8", errors="ignore")
        else:
            name = name_bytes.decode("utf-8", errors="ignore")

        file_size = struct.unpack("<I", entry[32:36])[0]
        file_offset = struct.unpack("<I", entry[36:40])[0]
        width = struct.unpack("<H", entry[40:42])[0]
        height = struct.unpack("<H", entry[42:44])[0]

        entries.append(
            {
                "name": name,
                "size": file_size,
                "offset": file_offset,
                "width": width,
                "height": height,
            }
        )

        print(f"Entry {i}: '{name}'")
        print(
            f"  size={file_size} ({file_size / 1024:.1f} KB), offset={file_offset}, w={width}, h={height}"
        )

    # Data section starts right after mmap table
    data_start = pos
    print(f"\nData section starts at byte: {data_start}")

    # Verify: check for 0x5A5A prefix
    print(
        f"First 4 bytes at data_start: 0x{data_start:02X} = {data[data_start : data_start + 4].hex()}"
    )

    # Extract each file
    print(f"\nExtracting files:")
    for i, e in enumerate(entries):
        # Each file in merged_data has 2-byte 0x5A5A prefix
        file_data_start = data_start + e["offset"] + 2
        file_data_end = file_data_start + e["size"]

        if file_data_end > len(data):
            print(f"  WARNING: {e['name']} extends beyond file bounds!")
            print(
                f"    data_start={data_start}, offset={e['offset']}, size={e['size']}"
            )
            print(
                f"    file_data_start={file_data_start}, file_data_end={file_data_end}, file_size={len(data)}"
            )
            continue

        file_data = data[file_data_start:file_data_end]

        output_path = os.path.join(output_dir, e["name"])
        with open(output_path, "wb") as f:
            f.write(file_data)

        print(
            f"  Extracted: {e['name']} -> {len(file_data)} bytes ({os.path.getsize(output_path)} on disk)"
        )

    return entries


if __name__ == "__main__":
    import sys

    if len(sys.argv) < 2:
        print("Usage: extract_assets.py <assets.bin> [output_dir]")
        sys.exit(1)

    assets_path = sys.argv[1]
    output_dir = sys.argv[2] if len(sys.argv) > 2 else None

    if not os.path.exists(assets_path):
        print(f"Error: {assets_path} not found!")
        sys.exit(1)

    extract_assets(assets_path, output_dir)
