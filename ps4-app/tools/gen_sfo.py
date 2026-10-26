#!/usr/bin/env python3
"""
PS4 param.sfo Generator (Pure Python, Zero External Dependencies)
Creates compliant Sony System File Object (SFO) binaries for PS4 Fake PKGs (fPKG).
"""

import sys
import struct
import os

SFO_MAGIC = b"\x00PSF"
SFO_VERSION = 0x00000101  # 1.1

FMT_UTF8_NULL = 0x0204
FMT_UINT32    = 0x0404

def build_sfo(entries):
    """
    entries: list of (key, fmt, value, max_len)
    """
    # Sort entries by key (SFO specification requires alphabetical key ordering)
    entries.sort(key=lambda x: x[0])
    
    num_entries = len(entries)
    header_size = 20
    index_table_size = num_entries * 16
    key_table_offset = header_size + index_table_size
    
    # Build key table
    key_table = bytearray()
    key_offsets = []
    for key, _, _, _ in entries:
        key_offsets.append(len(key_table))
        key_table.extend(key.encode('utf-8') + b'\x00')
        
    # Align data table offset to 4 bytes
    data_table_offset = (key_table_offset + len(key_table) + 3) & ~3
    key_table_padded_len = data_table_offset - key_table_offset
    key_table.extend(b'\x00' * (key_table_padded_len - len(key_table)))
    
    # Build data table and index entries
    data_table = bytearray()
    index_entries = []
    
    for i, (key, fmt, val, max_len) in enumerate(entries):
        cur_data_offset = len(data_table)
        key_offset = key_offsets[i]
        
        if fmt == FMT_UINT32:
            data = struct.pack('<I', int(val))
            actual_len = 4
            allocated_len = 4
        elif fmt == FMT_UTF8_NULL:
            str_bytes = str(val).encode('utf-8') + b'\x00'
            actual_len = len(str_bytes)
            allocated_len = max(actual_len, max_len)
            data = str_bytes + (b'\x00' * (allocated_len - actual_len))
        else:
            raise ValueError(f"Unsupported format: {fmt}")
            
        data_table.extend(data)
        # Pad data entry to 4 bytes
        while len(data_table) % 4 != 0:
            data_table.extend(b'\x00')
            
        index_entries.append((key_offset, fmt, actual_len, allocated_len, cur_data_offset))
        
    # Build final binary
    header = struct.pack('<4sIII I',
                         SFO_MAGIC,
                         SFO_VERSION,
                         key_table_offset,
                         data_table_offset,
                         num_entries)
                         
    index_bytes = bytearray()
    for ko, fmt, alen, mlen, do in index_entries:
        index_bytes.extend(struct.pack('<HHIII', ko, fmt, alen, mlen, do))
        
    return header + index_bytes + key_table + data_table

def create_default_ps4_sfo(title="PS4 Direct Downloader", title_id="PSFT00001", version="01.00", output_path="param.sfo"):
    content_id = f"IV0001-{title_id}_00-PS4DOWNLOADER000"
    entries = [
        ("APP_TYPE",            FMT_UINT32,    1,                  4),
        ("APP_VER",             FMT_UTF8_NULL, version,            8),
        ("ATTRIBUTE",           FMT_UINT32,    0,                  4),
        ("CATEGORY",            FMT_UTF8_NULL, "gd",               4),
        ("CONTENT_ID",          FMT_UTF8_NULL, content_id,         48),
        ("DOWNLOAD_DATA_SIZE",  FMT_UINT32,    0,                  4),
        ("SYSTEM_VER",          FMT_UINT32,    0x05050000,         4),  # Min FW 5.05
        ("TITLE",               FMT_UTF8_NULL, title,              128),
        ("TITLE_ID",            FMT_UTF8_NULL, title_id,           16),
        ("VERSION",             FMT_UTF8_NULL, version,            8),
    ]
    
    sfo_data = build_sfo(entries)
    os.makedirs(os.path.dirname(os.path.abspath(output_path)), exist_ok=True)
    with open(output_path, "wb") as f:
        f.write(sfo_data)
    print(f"[OK] param.sfo generated at {output_path} (Title: {title}, ID: {title_id})")

if __name__ == "__main__":
    out = sys.argv[1] if len(sys.argv) > 1 else "sce_sys/param.sfo"
    create_default_ps4_sfo(output_path=out)
