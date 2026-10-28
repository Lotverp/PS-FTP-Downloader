#!/usr/bin/env python3
"""
Test Suite: Archive Tools & Extraction Pipeline
Verifies that archive extraction tools (7z, unar, etc.) are available and functional
for multi-part / split archive extraction.
"""

import sys
import os
import shutil
import subprocess
import tempfile
import zipfile

def test_tool_availability():
    print("  -> Testing archive extraction binaries...")
    candidates = ["7z", "7zz", "unar", "7za", "unrar"]
    found = {}
    for c in candidates:
        p = shutil.which(c)
        if p:
            found[c] = p
            print(f"    [OK] Found tool: {c} -> {p}")

    if not found:
        print("FAIL: No archive extraction tool found (checked: 7z, 7zz, unar, 7za, unrar)")
        return False
    return True

def test_extraction_pipeline():
    print("  -> Testing extraction functionality with sample archive...")
    tool = shutil.which("7z") or shutil.which("7zz") or shutil.which("unar")
    if not tool:
        print("FAIL: No supported extractor available for functional test")
        return False

    temp_dir = tempfile.mkdtemp(prefix="p2c_test_arch_")
    try:
        # Create a test zip archive with a sample payload
        zip_path = os.path.join(temp_dir, "sample.zip")
        payload_name = "test_game.pkg"
        payload_content = b"\x7F\x46\x49\x52\x00\x01P2C_TEST_PAYLOAD_DATA_12345"

        with zipfile.ZipFile(zip_path, "w") as zf:
            zf.writestr(payload_name, payload_content)

        extract_dest = os.path.join(temp_dir, "extracted")
        os.makedirs(extract_dest, exist_ok=True)

        if "7z" in os.path.basename(tool):
            cmd = [tool, "x", "-y", f"-o{extract_dest}", zip_path]
        else: # unar
            cmd = [tool, "-f", "-o", extract_dest, zip_path]

        res = subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if res.returncode != 0:
            print(f"FAIL: Extractor {tool} failed with exit code {res.returncode}")
            return False

        extracted_file = os.path.join(extract_dest, payload_name)
        if not os.path.isfile(extracted_file):
            print(f"FAIL: Expected extracted file {extracted_file} not found")
            return False

        with open(extracted_file, "rb") as f:
            read_data = f.read()

        if read_data != payload_content:
            print("FAIL: Extracted payload content does not match original")
            return False

        print(f"    [OK] Extracted and verified archive using {os.path.basename(tool)}")
        return True

    finally:
        shutil.rmtree(temp_dir, ignore_errors=True)

def main():
    print("--- [TEST 4/5] Validating Archive Tools & Extraction Pipeline ---")
    if not test_tool_availability():
        sys.exit(1)
    if not test_extraction_pipeline():
        sys.exit(1)

    print("  [PASS] Archive extraction pipeline verified successfully!\n")
    sys.exit(0)

if __name__ == "__main__":
    main()
