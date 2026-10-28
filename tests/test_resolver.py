#!/usr/bin/env python3
"""
Test Suite: Resolver & Browser Download Safety
Verifies that undetected-chromedriver is operational and that browser downloads
are strictly prevented from saving to ~/Downloads.
"""

import sys
import os
import json
import subprocess

REPO_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
RESOLVER_SCRIPT = os.path.join(REPO_DIR, "gtk-app", "uc_resolver.py")

# Add gtk-app to path to import resolver modules directly
sys.path.insert(0, os.path.join(REPO_DIR, "gtk-app"))

def test_resolver_check():
    print("  -> Testing uc_resolver.py --check...")
    res = subprocess.run([sys.executable, RESOLVER_SCRIPT, "--check"], capture_output=True, text=True)
    if res.returncode != 0:
        print(f"FAIL: uc_resolver.py --check returned {res.returncode}: {res.stderr}")
        return False

    try:
        data = json.loads(res.stdout)
    except Exception as e:
        print(f"FAIL: uc_resolver.py did not return valid JSON: {res.stdout}")
        return False

    if not data.get("has_uc"):
        print("FAIL: undetected_chromedriver not detected by resolver")
        return False

    if not data.get("browser") or not os.path.exists(data["browser"]):
        print(f"FAIL: browser executable not found: {data.get('browser')}")
        return False

    if not data.get("ready"):
        print("FAIL: resolver ready flag is False")
        return False

    print(f"    [OK] Resolver operational with browser: {data['browser']} (uc: {data.get('uc_version')})")
    return True

def test_browser_download_prevention_code():
    print("  -> Testing browser download prevention security guards...")
    with open(RESOLVER_SCRIPT, "r", encoding="utf-8") as f:
        src = f.read()

    # 1. /dev/null fallback check (must NOT exist)
    if '"download.default_directory": "/dev/null"' in src:
        print("FAIL: uc_resolver.py still uses invalid /dev/null download directory!")
        return False
    print("    [OK] Dangerous /dev/null download directory is removed")

    # 2. temp sandbox directory check
    if "temp_dl_dir = tempfile.mkdtemp" not in src:
        print("FAIL: uc_resolver.py missing isolated temporary download sandbox directory")
        return False
    print("    [OK] Isolated download sandbox directory configured")

    # 3. CDP deny download check
    if "Browser.setDownloadBehavior" not in src or "Page.setDownloadBehavior" not in src:
        print("FAIL: uc_resolver.py missing CDP setDownloadBehavior guards")
        return False
    if '"behavior": "deny"' not in src and "'behavior': 'deny'" not in src:
        print("FAIL: uc_resolver.py CDP behavior is not set to 'deny'")
        return False
    print("    [OK] CDP Browser.setDownloadBehavior and Page.setDownloadBehavior configured to 'deny'")
    return True

def test_url_classification():
    print("  -> Testing resolver URL classification rules...")
    import uc_resolver

    # Protectors
    assert uc_resolver.is_protector_url("https://downloadgameps3.net/archives/50580"), "downloadgameps3.net protector"
    assert uc_resolver.is_protector_url("https://filecrypt.cc/Container/123.html"), "filecrypt protector"
    assert uc_resolver.is_protector_url("https://dlpsgame.com/archives/123"), "dlpsgame protector"
    assert not uc_resolver.is_protector_url("https://mediafire.com/file/123"), "mediafire is not protector"
    print("    [OK] Protector URLs classified correctly")

    # Hosters
    assert uc_resolver.is_hoster_landing_page("https://mediafire.com/file/123"), "mediafire hoster"
    assert uc_resolver.is_hoster_landing_page("https://akirabox.to/file/123"), "akirabox hoster"
    assert uc_resolver.is_hoster_landing_page("https://vikingfile.com/d/123"), "vikingfile hoster"
    assert uc_resolver.is_hoster_landing_page("https://1fichier.com/?abc"), "1fichier hoster"
    print("    [OK] Hoster landing pages classified correctly")

    # Direct streams
    assert uc_resolver.is_direct_stream_url("https://akirabox.com/download/hash/game.pkg"), "akirabox direct stream"
    assert uc_resolver.is_direct_stream_url("https://download1234.mediafire.com/hash/game.pkg"), "mediafire direct stream"
    assert uc_resolver.is_direct_stream_url("https://pixeldrain.com/api/file/xyz"), "pixeldrain direct stream"
    assert uc_resolver.is_direct_stream_url("https://alpha.staysharpsleeps8hours.xyz/MTc5MzIyMTcyMDp3Y3RDVWVLRGQ3TkdOd21kQTBmeXFROjY3MzAvYmFscGhhMjYvN2IzYzYxY2I1ZThiOGRmMTgxNzgvJTVCRExQU0dBTUUuQ09NJTVELU1fNTQzMDhfT1BPSVNTTzg5My5yYXI"), "staysharpsleeps8hours.xyz direct stream"
    assert uc_resolver.is_direct_stream_url("https://fishoilenjoyer.xyz/balpha26/test.rar"), "fishoilenjoyer.xyz direct stream"
    assert uc_resolver.is_direct_stream_url("https://fileditchfiles.st/balpha26/test.rar"), "fileditchfiles.st direct stream"
    assert not uc_resolver.is_direct_stream_url("https://akirabox.com/file/xyz"), "akirabox landing is not stream"
    print("    [OK] Direct streams classified correctly")

    # Direct file extensions
    assert uc_resolver.is_direct_file_url("https://example.com/game.pkg"), ".pkg direct file"
    assert uc_resolver.is_direct_file_url("https://example.com/archive.rar"), ".rar direct file"
    assert uc_resolver.is_direct_file_url("https://example.com/archive.7z"), ".7z direct file"
    assert uc_resolver.is_direct_file_url("https://example.com/archive.zip"), ".zip direct file"
    assert uc_resolver.is_direct_file_url("https://example.com/game.pkg?token=123"), "query string preserved"
    assert not uc_resolver.is_direct_file_url("https://example.com/page.html"), ".html is not direct file"
    print("    [OK] Direct file extensions classified correctly")

    return True

def main():
    print("--- [TEST 3/5] Validating Resolver & Browser Download Safety ---")
    if not test_resolver_check():
        sys.exit(1)
    if not test_browser_download_prevention_code():
        sys.exit(1)
    if not test_url_classification():
        sys.exit(1)

    print("  [PASS] Resolver & browser download safety verified successfully!\n")
    sys.exit(0)

if __name__ == "__main__":
    main()
