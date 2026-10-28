#!/usr/bin/env python3
"""
Test Suite: Catalog Integrity & Reachability
Validates catalog.json syntax, schema, required keys, and HTTP accessibility of icons and download URLs.
"""

import sys
import os
import json
import urllib.request
import urllib.error

CATALOG_PATH = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "catalog.json"))

def check_url(url, timeout=10, is_icon=False):
    headers = {
        'User-Agent': 'Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36'
    }
    if not is_icon:
        headers['Range'] = 'bytes=0-10'
    try:
        req = urllib.request.Request(url, headers=headers)
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return resp.status in (200, 206, 301, 302, 303, 307), resp.status, None
    except urllib.error.HTTPError as e:
        return False, e.code, str(e)
    except Exception as e:
        return False, None, str(e)

def main():
    print("--- [TEST 1/5] Validating catalog.json Schema & Reachability ---")
    if not os.path.isfile(CATALOG_PATH):
        print(f"FAIL: catalog.json not found at {CATALOG_PATH}")
        sys.exit(1)

    with open(CATALOG_PATH, "r", encoding="utf-8") as f:
        try:
            data = json.load(f)
        except Exception as e:
            print(f"FAIL: catalog.json is not valid JSON: {e}")
            sys.exit(1)

    items = data.get("items")
    if not isinstance(items, list) or len(items) == 0:
        print("FAIL: catalog.json has no valid 'items' list")
        sys.exit(1)

    print(f"  Found {len(items)} items in catalog '{data.get('name', 'Catalog')}'.")

    required_keys = ["id", "name", "version", "category", "region", "url", "pkg_name"]
    errors = []

    for idx, item in enumerate(items):
        item_name = item.get("name", f"Item #{idx}")
        for k in required_keys:
            val = item.get(k)
            if val is None or (isinstance(val, str) and not val.strip()):
                errors.append(f"Item '{item_name}' missing required field: '{k}'")

        size = item.get("size")
        if size is not None and (not isinstance(size, (int, float)) or size <= 0):
            errors.append(f"Item '{item_name}' has invalid size: {size}")

        icon_url = item.get("icon")
        if icon_url:
            ok, status, err = check_url(icon_url, is_icon=True)
            if not ok:
                errors.append(f"Item '{item_name}' icon URL 404/unreachable ({status}): {icon_url} ({err})")
            else:
                print(f"    [OK] Icon verified: {item_name} ({status})")

        dl_url = item.get("url")
        if dl_url:
            ok, status, err = check_url(dl_url, is_icon=False)
            if not ok:
                errors.append(f"Item '{item_name}' download URL unreachable ({status}): {dl_url} ({err})")
            else:
                print(f"    [OK] Download URL verified: {item_name} ({status})")

    if errors:
        print("\nERRORS DETECTED in catalog.json:")
        for e in errors:
            print(f"  - {e}")
        sys.exit(1)

    print("  [PASS] All catalog items and URLs verified successfully!\n")
    sys.exit(0)

if __name__ == "__main__":
    main()
