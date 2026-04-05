#!/usr/bin/env python3
"""Patch assets_final.bin to increase multinet threshold from 0.2 to 0.6."""

import struct, json

path = r"C:\Users\administered\Downloads\assets_final.bin"
with open(path, "rb") as f:
    data = bytearray(f.read())

total_files = struct.unpack_from("<I", data, 0)[0]
data_start = 12 + total_files * 44

# Find index.json entry
for i in range(total_files):
    off = 12 + i * 44
    name = data[off : off + 32].rstrip(b"\x00").decode("utf-8", errors="ignore")
    if name == "index.json":
        size = struct.unpack_from("<I", data, off + 32)[0]
        offset = struct.unpack_from("<I", data, off + 36)[0]
        file_offset = data_start + offset + 2
        json_bytes = data[file_offset : file_offset + size]
        idx = json.loads(json_bytes)

        old_threshold = idx["multinet_model"]["threshold"]
        idx["multinet_model"]["threshold"] = 0.6

        new_json = json.dumps(idx, indent=4, ensure_ascii=False).encode("utf-8")

        if len(new_json) <= size:
            # Pad to same size
            new_json = new_json + b"\x00" * (size - len(new_json))
            data[file_offset : file_offset + size] = new_json

            # Recalculate checksum
            mmap_table = data[12 : 12 + total_files * 44]
            merged_data = data[12 + total_files * 44 :]
            combined = bytes(mmap_table) + bytes(merged_data)
            new_checksum = sum(combined) & 0xFFFF

            struct.pack_into("<I", data, 4, new_checksum)

            with open(path, "wb") as f:
                f.write(data)

            print(f"Threshold: {old_threshold} -> 0.6")
            print(f"New checksum: 0x{new_checksum:04X}")
            print("Done!")
        else:
            print(f"ERROR: new json ({len(new_json)}) larger than old ({size})")
        break
