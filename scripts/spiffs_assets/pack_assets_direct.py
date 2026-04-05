#!/usr/bin/env python3
"""
Pack assets directly into mmap_assets format.
Matches the exact format used by spiffs_assets_gen.py.
"""

import struct
import json
import os
import shutil


def compute_checksum(data):
    return sum(data) & 0xFFFF


# Working directory
temp_dir = r"E:\Code\ESP32\SF\xiaozhi-esp32\scripts\spiffs_assets\build\assets_final"
original_assets = r"C:\Users\administered\Downloads\assets.bin"
output_path = r"C:\Users\administered\Downloads\assets_final.bin"

# Clean prepare
if os.path.exists(temp_dir):
    shutil.rmtree(temp_dir)
os.makedirs(temp_dir)

# Step 1: Extract original assets
print("=== Step 1: Extract original assets ===")
with open(original_assets, "rb") as f:
    orig_data = f.read()

orig_total = struct.unpack_from("<I", orig_data, 0)[0]
data_start = 12 + orig_total * 44

orig_entries = []
for i in range(orig_total):
    off = 12 + i * 44
    name = orig_data[off : off + 32].rstrip(b"\x00").decode("utf-8", errors="ignore")
    size = struct.unpack_from("<I", orig_data, off + 32)[0]
    offset = struct.unpack_from("<I", orig_data, off + 36)[0]
    orig_entries.append({"name": name, "size": size, "offset": offset})

for entry in orig_entries:
    name = entry["name"]
    offset = entry["offset"]
    size = entry["size"]
    file_abs_offset = data_start + offset + 2  # +2 for 0x5A5A prefix
    file_data = orig_data[file_abs_offset : file_abs_offset + size]
    with open(os.path.join(temp_dir, name), "wb") as f:
        f.write(file_data)
    print(f"  {name}: {size} bytes")

# Step 2: Copy EAF emoji files
print("\n=== Step 2: Copy EAF emoji files ===")
emoji_large = r"E:\Code\ESP32\SF\xiaozhi-esp32\managed_components\espressif2022__esp_emote_gfx\emoji_large"
emote_json_path = r"E:\Code\ESP32\SF\xiaozhi-esp32\main\boards\echoear\emote.json"
with open(emote_json_path, "r") as f:
    emote_config = json.load(f)

emote_srcs = set()
for emote in emote_config:
    emote_srcs.add(emote["src"])

for eaf_name in sorted(emote_srcs):
    src = os.path.join(emoji_large, eaf_name)
    if os.path.exists(src):
        dst = os.path.join(temp_dir, eaf_name)
        shutil.copy2(src, dst)
        print(f"  {eaf_name}: {os.path.getsize(dst)} bytes")

# Step 3: Copy icon files
print("\n=== Step 3: Copy icon files ===")
icon_files = [
    "battery_charge.bin",
    "battery_level1.bin",
    "battery_level2.bin",
    "battery_level3.bin",
    "battery_level4.bin",
    "icon_mic.bin",
    "icon_speaker.bin",
    "icon_tips.bin",
    "icon_WiFi_fail.bin",
    "icon_wifi_ok.bin",
    "listen.eaf",
]
for icon_name in icon_files:
    src = os.path.join(emoji_large, icon_name)
    if os.path.exists(src):
        dst = os.path.join(temp_dir, icon_name)
        shutil.copy2(src, dst)
        print(f"  {icon_name}: {os.path.getsize(dst)} bytes")

# Step 4: Build new index.json
print("\n=== Step 4: Build new index.json ===")
new_index = {
    "version": 1,
    "srmodels": "srmodels.bin",
    "text_font": "font_puhui_deepseek_20_4.bin",
    "multinet_model": {
        "language": "cn",
        "duration": 3000,
        "threshold": 0.45,
        "commands": [{"command": "ni hao ling yi", "text": "零一", "action": "wake"}],
    },
    "emoji_collection": [],
    "icon_collection": [],
    "layout": [],
}

for emote in emote_config:
    new_index["emoji_collection"].append(
        {
            "name": emote["emote"],
            "file": emote["src"],
            "eaf": {"loop": emote.get("loop", True), "fps": emote.get("fps", 20)},
        }
    )

icon_names_in_dir = [
    f
    for f in os.listdir(temp_dir)
    if f.startswith("icon_") or f.startswith("battery_") or f == "listen.eaf"
]
for icon_name in sorted(icon_names_in_dir):
    base = os.path.splitext(icon_name)[0]
    new_index["icon_collection"].append({"name": base, "file": icon_name})

new_index["layout"] = [
    {"name": "eye_anim", "align": "GFX_ALIGN_LEFT_MID", "x": 10, "y": 10},
    {"name": "status_icon", "align": "GFX_ALIGN_TOP_MID", "x": -100, "y": 38},
    {
        "name": "toast_label",
        "align": "GFX_ALIGN_TOP_MID",
        "x": 0,
        "y": 40,
        "width": 160,
        "height": 40,
    },
    {
        "name": "clock_label",
        "align": "GFX_ALIGN_TOP_MID",
        "x": 0,
        "y": 40,
        "width": 60,
        "height": 50,
    },
    {"name": "listen_anim", "align": "GFX_ALIGN_TOP_MID", "x": 0, "y": 25},
]

index_path = os.path.join(temp_dir, "index.json")
with open(index_path, "w", encoding="utf-8") as f:
    json.dump(new_index, f, indent=4, ensure_ascii=False)
print(
    f"  emoji: {len(new_index['emoji_collection'])}, icons: {len(new_index['icon_collection'])}"
)

# Step 5: Pack using spiffs_assets_gen.py format
print("\n=== Step 5: Pack assets ===")
merged_data = bytearray()
file_info_list = []
max_name_len = 32

all_files = sorted(os.listdir(temp_dir))
print(f"Packing {len(all_files)} files...")

for filename in all_files:
    file_path = os.path.join(temp_dir, filename)
    file_size = os.path.getsize(file_path)

    # Record offset BEFORE adding this file (offset is relative to merged_data start)
    current_offset = len(merged_data)

    # Add 0x5A5A prefix
    merged_data.extend(b"\x5a\x5a")

    # Read and append file data
    with open(file_path, "rb") as f:
        merged_data.extend(f.read())

    file_info_list.append((filename, current_offset, file_size, 0, 0))
    print(f"  {filename}: offset=0x{current_offset:X}, size={file_size}")

# Build mmap table
mmap_table = bytearray()
for file_name, offset, file_size, width, height in file_info_list:
    fixed_name = file_name.encode("utf-8").ljust(max_name_len, b"\x00")[:max_name_len]
    mmap_table.extend(fixed_name)
    mmap_table.extend(struct.pack("<I", file_size))
    mmap_table.extend(struct.pack("<I", offset))
    mmap_table.extend(struct.pack("<H", width))
    mmap_table.extend(struct.pack("<H", height))

total_files = len(file_info_list)
combined_data = bytes(mmap_table) + bytes(merged_data)
combined_checksum = compute_checksum(combined_data)
combined_data_length = len(combined_data)

header = (
    struct.pack("<I", total_files)
    + struct.pack("<I", combined_checksum)
    + struct.pack("<I", combined_data_length)
)
final_data = header + combined_data

print(f"\nTotal: {total_files} files")
print(f"Checksum: 0x{combined_checksum:04X}")
print(f"Final size: {len(final_data)} bytes ({len(final_data) / 1024:.1f} KB)")

# Verify checksum
verify_chksum = compute_checksum(final_data[12:])
print(
    f"Verify checksum: 0x{verify_chksum:04X} ({'OK' if verify_chksum == combined_checksum else 'MISMATCH'})"
)

# Write output
with open(output_path, "wb") as f:
    f.write(final_data)
print(f"\nWritten to: {output_path}")

# Step 6: Verify
print("\n=== Verification ===")
verify_data = final_data
v_total = struct.unpack_from("<I", verify_data, 0)[0]
v_chksum = struct.unpack_from("<I", verify_data, 4)[0]
v_len = struct.unpack_from("<I", verify_data, 8)[0]
print(f"Files: {v_total}, Checksum: 0x{v_chksum:04X}, Len: {v_len}")

v_data_start = 12 + v_total * 44
for i in range(v_total):
    off = 12 + i * 44
    name = verify_data[off : off + 32].rstrip(b"\x00").decode("utf-8", errors="ignore")
    size = struct.unpack_from("<I", verify_data, off + 32)[0]
    offset = struct.unpack_from("<I", verify_data, off + 36)[0]
    print(f"  [{i:2d}] {name:32s} size={size:8d} offset=0x{offset:06X}")

# Verify index.json has multinet_model
for i in range(v_total):
    off = 12 + i * 44
    name = verify_data[off : off + 32].rstrip(b"\x00").decode("utf-8", errors="ignore")
    if name == "index.json":
        size = struct.unpack_from("<I", verify_data, off + 32)[0]
        offset = struct.unpack_from("<I", verify_data, off + 36)[0]
        f_off = v_data_start + offset + 2  # Skip 0x5A5A
        idx_bytes = verify_data[f_off : f_off + size]
        idx_parsed = json.loads(idx_bytes)
        print(f"\nindex.json keys: {list(idx_parsed.keys())}")
        if "multinet_model" in idx_parsed:
            cmd = idx_parsed["multinet_model"]["commands"][0]["command"]
            print(f"multinet_model: FOUND! Command: {cmd}")
        else:
            print("multinet_model: NOT FOUND!")
        break

print("\nDone!")
