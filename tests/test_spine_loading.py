#!/usr/bin/env python3
"""
test_spine_loading.py — Validate all Spine assets in a data.pack can be loaded
without crashing. Simulates the C++ SpineViewer loading pipeline in Python.

Usage:
  python test_spine_loading.py <pack_dir>
  python test_spine_loading.py "G:\G_Games\Tools_Hacking_gamedev\EpicSevenAssetRipper2.03\Staging\com.smilegate.chaoszero.stove.google\files"
"""

import os
import sys
import struct
import re
from pathlib import Path

# ============================================================================
# XOR decryption (matches Core.h)
# ============================================================================

INITIAL = 0x24D1C
MULT = 0x41C64E6D
KEY_SIZE = 0x81

def generate_key():
    key = bytearray(KEY_SIZE)
    current = INITIAL
    for i in range(KEY_SIZE):
        current = (current * MULT) & 0x7FFFFFFF
        key[i] = (current >> 16) & 0xFF
    return bytes(key)

XOR_KEY = generate_key()

def xor_decrypt(data, file_offset):
    result = bytearray(data)
    for i in range(len(result)):
        result[i] ^= XOR_KEY[(file_offset + i) % KEY_SIZE]
    return bytes(result)

# ============================================================================
# LZ4 block decompression (matches SCSPParser.cpp)
# ============================================================================

def lz4_decompress_block(src, uncompressed_size):
    out = bytearray()
    i = 0
    while i < len(src):
        token = src[i]; i += 1
        lit_len = token >> 4
        if lit_len == 15:
            while i < len(src):
                b = src[i]; i += 1
                lit_len += b
                if b != 255: break
        if lit_len > 0:
            if i + lit_len > len(src): break
            out.extend(src[i:i+lit_len])
            i += lit_len
            if i >= len(src): break
        if i + 2 > len(src): break
        offset = src[i] | (src[i+1] << 8); i += 2
        if offset == 0: break
        match_len = (token & 0x0F) + 4
        if (token & 0x0F) == 15:
            while i < len(src):
                b = src[i]; i += 1
                match_len += b
                if b != 255: break
        start = len(out) - offset
        for j in range(match_len):
            out.append(out[start + j])
    return bytes(out[:uncompressed_size])

# ============================================================================
# Pack scanner — marker-based scan matching C++ ScanEncrypted
# ============================================================================

def is_likely_path(s):
    """Check if string looks like a valid file path"""
    if not s or len(s) < 3:
        return False
    try:
        s.encode("ascii")
    except:
        return False
    if any(c in s for c in "\x00\x01\x02\x03\x04\x05"):
        return False
    return bool(re.match(r'^[a-zA-Z0-9_/\-\.]+$', s))

def scan_pack_encrypted(pack_dir):
    """Scan encrypted pack files using marker-byte approach (matches C++ ScanEncrypted)"""
    base = os.path.join(pack_dir, "data.pack")
    if not os.path.exists(base):
        print(f"ERROR: {base} not found")
        sys.exit(1)

    # Find all pack parts
    parts = [base]
    i = 1
    while os.path.exists(f"{base}~{i}"):
        parts.append(f"{base}~{i}")
        i += 1
    print(f"Found {len(parts)} pack part(s)")

    # Concatenate parts logically — we track which part each offset maps to
    part_info = []  # (path, start_offset, size)
    total_size = 0
    for p in parts:
        sz = os.path.getsize(p)
        part_info.append((p, total_size, sz))
        total_size += sz
    print(f"Total pack size: {total_size / (1024*1024*1024):.2f} GB")

    def read_at(abs_offset, count):
        """Read bytes from the logical concatenated pack"""
        for path, start, sz in part_info:
            if abs_offset >= start and abs_offset < start + sz:
                local_off = abs_offset - start
                actual_read = min(count, sz - local_off)
                with open(path, "rb") as f:
                    f.seek(local_off)
                    return f.read(actual_read)
        return b""

    files = {}
    cursor = 4
    CHUNK_SIZE = 4 * 1024 * 1024  # 4MB scan chunks
    found = 0

    print("Scanning for file entries...")
    while cursor < total_size:
        if cursor % (100 * 1024 * 1024) == 0:
            pct = cursor / total_size * 100
            print(f"  {pct:.0f}% ({found} files found)...", end="\r")

        # Read a chunk and scan for marker byte 0x02
        chunk = read_at(cursor, CHUNK_SIZE)
        if not chunk:
            break

        pos = 0
        while pos < len(chunk):
            # Decrypt single byte to check for 0x02 marker
            abs_pos = cursor + pos
            decrypted = chunk[pos] ^ XOR_KEY[abs_pos % KEY_SIZE]

            if decrypted != 0x02:
                pos += 1
                continue

            # Found potential marker at abs_pos
            # Header starts 4 bytes before the marker
            header_offset = abs_pos - 4
            if header_offset < 0:
                pos += 1
                continue

            # Read 15-byte header
            raw_header = read_at(header_offset, 15)
            if len(raw_header) < 15:
                pos += 1
                continue

            header = bytearray(xor_decrypt(raw_header, header_offset))
            container_len = struct.unpack_from("<I", header, 0)[0]
            path_len = header[5]
            data_len = struct.unpack_from("<I", header, 6)[0]

            # Validate
            if (container_len > total_size or path_len == 0 or path_len > 255
                or data_len > total_size
                or container_len != path_len + data_len + 19):
                pos += 1
                continue

            if header_offset + 15 + path_len + data_len > total_size:
                pos += 1
                continue

            # Read path
            raw_path = read_at(header_offset + 15, path_len)
            if len(raw_path) < path_len:
                pos += 1
                continue

            path_bytes = xor_decrypt(raw_path, header_offset + 15)
            try:
                file_path = path_bytes.decode("utf-8").rstrip("\x00").replace("\\", "/").strip("/")
            except:
                pos += 1
                continue

            if not is_likely_path(file_path):
                pos += 1
                continue

            file_data_offset = header_offset + 15 + path_len
            files[file_path] = (file_data_offset, data_len)
            found += 1

            # Skip past this entry
            skip = (header_offset + container_len) - cursor
            if skip > pos:
                pos = skip
            else:
                pos += 1
            continue

        cursor += len(chunk)

    print(f"\nScan complete: {len(files)} files found")
    return files, part_info

def read_file_data(files, part_info, path):
    """Read and decrypt file data"""
    if path not in files:
        return None
    offset, size = files[path]
    for ppath, start, sz in part_info:
        if offset >= start and offset < start + sz:
            local = offset - start
            with open(ppath, "rb") as f:
                f.seek(local)
                data = f.read(size)
            return xor_decrypt(data, offset)
    return None

# ============================================================================
# Validation functions
# ============================================================================

def validate_scsp(data):
    if len(data) < 8:
        return False, "Too small"
    dec_len = struct.unpack_from("<i", data, 0)[0]
    comp_len = struct.unpack_from("<i", data, 4)[0]
    if comp_len < 0 or dec_len <= 0:
        return False, "Invalid header"
    if 8 + comp_len > len(data):
        return False, "Compressed exceeds size"
    try:
        decompressed = lz4_decompress_block(data[8:8+comp_len], dec_len)
    except Exception as e:
        return False, f"LZ4 failed: {e}"
    if len(decompressed) < 0x62:
        return False, f"Decompressed too small ({len(decompressed)})"
    if decompressed[0x08:0x0C] != b"scsp":
        return False, f"Bad magic: {decompressed[0x08:0x0C]}"
    return True, f"OK ({len(decompressed)} bytes)"

def validate_atlas(data):
    try:
        text = data.decode("utf-8", errors="replace")
    except:
        return False, "Not text", []
    if len(text) < 50:
        return False, f"Too small ({len(text)})", []
    textures = []
    after_blank = True
    for line in text.split("\n"):
        line = line.rstrip("\r")
        if not line:
            after_blank = True
            continue
        if after_blank and line and not line[0] in " \t" and "." in line:
            textures.append(line)
        after_blank = False
    if not textures:
        return False, "No texture pages", []
    return True, f"OK ({len(textures)} tex)", textures

# ============================================================================
# Main
# ============================================================================

def run_tests(pack_dir):
    files, part_info = scan_pack_encrypted(pack_dir)

    scsp = {p for p in files if p.lower().endswith(".scsp")}
    atlas = {p for p in files if p.lower().endswith(".atlas")}
    print(f"SCSP: {len(scsp)}, Atlas: {len(atlas)}")

    results = {"total": 0, "pass": 0, "fail_scsp": 0, "fail_atlas": 0,
               "fail_atlas_small": 0, "fail_tex": 0, "failures": []}

    for sp in sorted(scsp):
        d = os.path.dirname(sp)
        bn = os.path.splitext(os.path.basename(sp))[0]
        ap = f"{d}/{bn}.atlas" if d else f"{bn}.atlas"
        if ap not in atlas:
            da = [a for a in atlas if os.path.dirname(a) == d]
            if len(da) == 1:
                ap = da[0]
            else:
                continue

        results["total"] += 1

        sd = read_file_data(files, part_info, sp)
        if not sd:
            results["fail_scsp"] += 1
            results["failures"].append((sp, "Can't read SCSP"))
            continue
        ok, msg = validate_scsp(sd)
        if not ok:
            results["fail_scsp"] += 1
            results["failures"].append((sp, f"SCSP: {msg}"))
            continue

        ad = read_file_data(files, part_info, ap)
        if not ad:
            results["fail_atlas"] += 1
            results["failures"].append((sp, "Can't read atlas"))
            continue
        ok, msg, texs = validate_atlas(ad)
        if not ok:
            if len(ad) < 50:
                results["fail_atlas_small"] += 1
            results["fail_atlas"] += 1
            results["failures"].append((sp, f"Atlas: {msg}"))
            continue

        results["pass"] += 1

    print("\n" + "=" * 60)
    print(f"TOTAL with atlas:    {results['total']}")
    print(f"PASS:                {results['pass']}")
    print(f"FAIL (SCSP):         {results['fail_scsp']}")
    print(f"FAIL (Atlas):        {results['fail_atlas']}")
    print(f"  - Atlas too small: {results['fail_atlas_small']}")
    print(f"FAIL (Textures):     {results['fail_tex']}")

    if results["failures"]:
        print(f"\nFirst 30 failures:")
        for p, r in results["failures"][:30]:
            print(f"  {p}: {r}")

    return results

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: python test_spine_loading.py <pack_directory>")
        sys.exit(1)
    run_tests(sys.argv[1])
