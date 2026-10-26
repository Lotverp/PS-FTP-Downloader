#!/usr/bin/env python3
"""
PS FTP Downloader — Undetected ChromeDriver Resolver
Resolves protected download links (Cloudflare, link shorteners, protectors)
and extracts direct download URLs with authenticated browser session headers/cookies.
"""

import sys
import os
import time
import re
import json
import argparse
import shutil
import random
import hashlib
import signal
import tempfile
import multiprocessing
try:
    multiprocessing.set_start_method("fork", force=True)
except Exception:
    pass
from concurrent.futures import ProcessPoolExecutor, as_completed
from urllib.parse import urlparse

# Protector / Shortener domains that list hosters rather than serving direct files
PROTECTOR_DOMAINS = [
    "downloadgameps3.net",
    "dlpsgame.com",
    "filecrypt.cc",
    "ouo.io",
    "linkvertise.com",
]

# Hoster landing pages (require extraction of real download button)
HOSTER_DOMAINS = [
    "mediafire.com",
    "1fichier.com",
    "pixeldrain.com",
    "mega.nz",
    "mega.co.nz",
    "drive.google.com",
    "akirabox.to",
    "akirabox.com",
    "vikingfile.com",
    "vik1ngfile.site",
    "rootz.so",
    "datanodes.to",
    "itsfileditchfiles.st",
    "fileditch.com",
    "rapidgator.net",
    "qiwi.gg",
    "gofile.io",
    "bowfile.com",
    "hexload.com",
]

# Direct CDN / File stream patterns that don't need further navigation
DIRECT_CDN_PATTERNS = [
    re.compile(r"fishoilenjoyer\.xyz/", re.IGNORECASE),
    re.compile(r"akirabox\.com/download/", re.IGNORECASE),
    re.compile(r"download\.akirabox\.com/", re.IGNORECASE),
    re.compile(r"vikingfile\.com/d/", re.IGNORECASE),
    re.compile(r"pixeldrain\.com/api/file/", re.IGNORECASE),
    re.compile(r"download\d*\.mediafire\.com/", re.IGNORECASE),
]

def find_browser():
    candidates = [
        "/snap/bin/chromium",
        "/usr/bin/google-chrome-stable",
        "/usr/bin/google-chrome",
        "/usr/bin/chromium-browser",
        "/usr/bin/chromium",
    ]
    for c in candidates:
        if os.path.isfile(c) and os.access(c, os.X_OK):
            return c
    for name in ["google-chrome", "google-chrome-stable", "chromium-browser", "chromium"]:
        which = shutil.which(name)
        if which:
            return which
    return None

def check_dependencies():
    browser = find_browser()
    try:
        import undetected_chromedriver as uc
        import selenium
        has_uc = True
        uc_ver = getattr(uc, "__version__", "unknown")
    except ImportError:
        has_uc = False
        uc_ver = None

    return {
        "has_uc": has_uc,
        "uc_version": uc_ver,
        "browser": browser,
        "ready": bool(has_uc and browser)
    }

def is_protector_url(url):
    if not url: return False
    netloc = urlparse(url).netloc.lower()
    return any(p in netloc for p in PROTECTOR_DOMAINS)

def is_hoster_landing_page(url):
    if not url: return False
    netloc = urlparse(url).netloc.lower()
    return any(h in netloc for h in HOSTER_DOMAINS)

def is_direct_stream_url(url):
    if not url: return False
    for p in DIRECT_CDN_PATTERNS:
        if p.search(url):
            return True
    return False

def is_direct_file_url(url):
    if not url or not url.startswith("http"): return False
    clean = url.split("?")[0].lower()
    return any(clean.endswith(ext) for ext in [".pkg", ".rar", ".7z", ".zip", ".iso", ".bin"])

def close_rogue_ad_windows(driver, main_handle=None):
    """Closes any rogue popup or ad windows that do not belong to legitimate hosters or protectors."""
    try:
        handles = list(driver.window_handles)
    except Exception:
        return
    for h in handles:
        if main_handle and h == main_handle:
            continue
        try:
            driver.switch_to.window(h)
            curr = (driver.current_url or "").lower()
            if not is_hoster_landing_page(curr) and not is_protector_url(curr) and not is_direct_stream_url(curr) and not is_direct_file_url(curr):
                driver.close()
        except Exception:
            pass
    if main_handle:
        try:
            if main_handle in driver.window_handles:
                driver.switch_to.window(main_handle)
        except Exception:
            pass

def verify_download_link(url, cookie_str=None, user_agent=None, referer=None, timeout=6):
    """Performs a quick HTTP check to verify the URL does not return 404/410/403/500."""
    import urllib.request
    try:
        headers = {
            'User-Agent': user_agent or 'Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36',
            'Range': 'bytes=0-10'
        }
        if referer:
            headers['Referer'] = referer
        if cookie_str:
            headers['Cookie'] = cookie_str
        req = urllib.request.Request(url, headers=headers)
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return resp.status in (200, 206, 301, 302, 303, 307)
    except urllib.error.HTTPError as e:
        if e.code in (404, 410, 403, 500, 502):
            return False
        return e.code not in (404, 410)
    except Exception:
        return True

# ── Intercettazione download ──────────────────────────────────────────────
# Il browser del resolver NON deve mai salvare file sul PC: ogni download viene
# negato via CDP e il suo URL viene recuperato dal log DevTools, così il file
# viene scaricato dall'app e inviato direttamente alla PlayStation via FTP.

BINARY_MIME_HINTS = (
    "application/octet-stream", "binary/octet-stream", "application/x-rar",
    "application/vnd.rar", "application/zip", "application/x-zip",
    "application/x-7z", "application/x-iso", "application/force-download",
    "application/download", "application/x-download", "application/x-msdownload",
)

SKIP_RESOURCE_EXT = (
    ".js", ".css", ".png", ".jpg", ".jpeg", ".gif", ".webp", ".svg", ".ico",
    ".woff", ".woff2", ".ttf", ".otf", ".json", ".wasm", ".mp4", ".webm",
    ".m3u8", ".ts", ".map",
)

MIN_FILE_SIZE = 512 * 1024

def _headers_lower(headers):
    try:
        return {str(k).lower(): str(v) for k, v in (headers or {}).items()}
    except Exception:
        return {}

def response_is_file(status, headers, mime, url):
    """True se la risposta HTTP è un file scaricabile (e non una pagina o una risorsa)."""
    if not url or not url.startswith("http"):
        return False
    try:
        status = int(status or 0)
    except (TypeError, ValueError):
        status = 0
    if status not in (200, 206):
        return False
    h = _headers_lower(headers)
    ctype = (mime or h.get("content-type", "")).lower()
    if ("html" in ctype or "javascript" in ctype or "json" in ctype or
            ctype.startswith("image/") or ctype.startswith("text/") or
            ctype.startswith("video/") or ctype.startswith("audio/") or
            ctype.startswith("font/")):
        return False
    if url.split("?")[0].lower().endswith(SKIP_RESOURCE_EXT):
        return False

    size = -1
    cr = h.get("content-range", "")
    if "/" in cr and cr.rsplit("/", 1)[1].strip().isdigit():
        size = int(cr.rsplit("/", 1)[1].strip())
    elif h.get("content-length", "").strip().isdigit():
        size = int(h["content-length"].strip())

    disp = h.get("content-disposition", "").lower()
    if "attachment" in disp or "filename" in disp or is_direct_file_url(url):
        return size < 0 or size >= MIN_FILE_SIZE
    if any(m in ctype for m in BINARY_MIME_HINTS):
        return size >= 5 * 1024 * 1024
    return False

def block_browser_downloads(driver):
    """Impedisce al browser di salvare qualsiasi file su disco."""
    for cmd, params in (
        ("Browser.setDownloadBehavior", {"behavior": "deny", "eventsEnabled": True}),
        ("Page.setDownloadBehavior", {"behavior": "deny"}),
    ):
        try:
            driver.execute_cdp_cmd(cmd, params)
        except Exception:
            pass

class DownloadSniffer:
    """Legge il performance log di ChromeDriver e cattura l'URL di ogni download avviato
    (click su pulsanti, redirect, finestre popup) prima che il browser lo blocchi."""

    def __init__(self, driver):
        self.driver = driver
        self.req_urls = {}
        self.found = None

    def poll(self):
        if self.found:
            return self.found
        try:
            entries = self.driver.get_log("performance")
        except Exception:
            return None
        for entry in entries:
            try:
                msg = json.loads(entry["message"])["message"]
            except Exception:
                continue
            method = msg.get("method", "")
            p = msg.get("params") or {}
            cand = None
            if method in ("Page.downloadWillBegin", "Browser.downloadWillBegin"):
                u = p.get("url") or ""
                if u.startswith("http"):
                    cand = u
            elif method == "Network.requestWillBeSent":
                rid = p.get("requestId")
                u = (p.get("request") or {}).get("url") or ""
                if rid and u:
                    self.req_urls[rid] = u
            elif method == "Network.responseReceived":
                resp = p.get("response") or {}
                u = resp.get("url") or self.req_urls.get(p.get("requestId"), "")
                if response_is_file(resp.get("status"), resp.get("headers"), resp.get("mimeType"), u):
                    cand = u
            elif method == "Network.responseReceivedExtraInfo":
                u = self.req_urls.get(p.get("requestId"), "")
                if u and response_is_file(p.get("statusCode"), p.get("headers"), None, u):
                    cand = u
            if cand and not is_protector_url(cand):
                self.found = cand
                return cand
        return None

def collect_session(driver, target_url):
    """Cookie validi per l'URL di download, User-Agent e Referer della sessione del browser."""
    cookie_str = ""
    try:
        res = driver.execute_cdp_cmd("Network.getCookies", {"urls": [target_url]})
        cookie_str = "; ".join(f"{c['name']}={c['value']}" for c in res.get("cookies", []))
    except Exception:
        try:
            cookie_str = "; ".join(f"{c['name']}={c['value']}" for c in driver.get_cookies())
        except Exception:
            pass
    user_agent = ""
    try:
        user_agent = driver.execute_script("return navigator.userAgent;") or ""
    except Exception:
        pass
    referer = ""
    try:
        cur = driver.current_url or ""
        if cur.startswith("http"):
            referer = cur
    except Exception:
        pass
    return cookie_str, user_agent, referer

def success_result(driver, url, source, password):
    cookie_str, user_agent, referer = collect_session(driver, url)
    title = ""
    try:
        title = driver.title or ""
    except Exception:
        pass
    return {
        "success": True,
        "url": url,
        "title": title,
        "cookie": cookie_str,
        "user_agent": user_agent,
        "referer": referer,
        "password": password or "",
        "source": source,
    }

def wait_for_cloudflare(driver, timeout=20):
    """Wait for Cloudflare Turnstile / Managed Challenge to complete."""
    from selenium.webdriver.common.by import By
    start = time.time()
    while time.time() - start < timeout:
        title = (driver.title or "").lower()
        if "just a moment" not in title and "cloudflare" not in title and "checking your browser" not in title:
            return True

        # Try to click turnstile checkbox if visible in iframes
        try:
            cf_frames = driver.find_elements(By.CSS_SELECTOR, "iframe[src*='challenges.cloudflare.com']")
            for frame in cf_frames:
                driver.switch_to.frame(frame)
                boxes = driver.find_elements(By.CSS_SELECTOR, "input[type='checkbox'], #challenge-stage")
                if boxes:
                    try:
                        boxes[0].click()
                    except Exception:
                        pass
                driver.switch_to.default_content()
        except Exception:
            try:
                driver.switch_to.default_content()
            except Exception:
                pass

        time.sleep(1.0)
    return False

def solve_sha1_range(prefix_str, difficulty, start_nonce, step, batch=100000):
    prefix = prefix_str.encode("ascii")
    mask_bytes = difficulty // 8
    target = b"\x00" * mask_bytes
    rem_bits = difficulty % 8
    n = start_nonce
    while True:
        for _ in range(batch):
            h = hashlib.sha1(prefix + str(n).encode("ascii")).digest()
            if h[:mask_bytes] == target:
                if rem_bits == 0:
                    return n
                elif (h[mask_bytes] >> (8 - rem_bits)) == 0:
                    return n
            n += step

def fast_solve_sha1_pow(challenge, difficulty, max_workers=None):
    if max_workers is None:
        max_workers = os.cpu_count() or 4
    prefix_str = f"{challenge}:"
    with ProcessPoolExecutor(max_workers=max_workers) as executor:
        futures = [
            executor.submit(solve_sha1_range, prefix_str, difficulty, i, max_workers)
            for i in range(max_workers)
        ]
        for f in as_completed(futures):
            res = f.result()
            executor.shutdown(wait=False, cancel_futures=True)
            return res

def handle_filecrypt(driver, timeout=35, password=None):
    """Handles filecrypt container pages: pow-captcha, password submission, and container unlocking."""
    from selenium.webdriver.common.by import By

    start = time.time()
    main_handle = driver.current_window_handle

    def try_submit_password():
        if not password:
            return False
        try:
            pass_inps = driver.find_elements(By.CSS_SELECTOR, "#p4assw0rt, input[name='password'], input[type='password']")
            vis_inps = [p for p in pass_inps if p.is_displayed()]
            if vis_inps:
                vis_inps[0].clear()
                vis_inps[0].send_keys(password)
                submits = driver.find_elements(By.CSS_SELECTOR, "button, input[type='submit']")
                for s in submits:
                    if s.is_displayed():
                        s.click()
                        time.sleep(2.0)
                        close_rogue_ad_windows(driver, main_handle)
                        return True
        except Exception:
            pass
        return False

    # 1. Handle password form if present initially
    try_submit_password()

    # 2. Click CutCaptcha / PoW checkbox with native trusted CDP click
    try:
        boxes = driver.find_elements(By.CSS_SELECTOR, ".pow-captcha__box, .pow-captcha")
        vis_boxes = [b for b in boxes if b.is_displayed()]
        if vis_boxes:
            box = vis_boxes[0]
            try:
                rect = driver.execute_script("return arguments[0].getBoundingClientRect();", box)
                x = int(rect['x'] + rect['width'] / 2)
                y = int(rect['y'] + rect['height'] / 2)
                driver.execute_cdp_cmd('Input.dispatchMouseEvent', {'type': 'mouseMoved', 'x': x - 40, 'y': y - 40})
                time.sleep(0.08)
                driver.execute_cdp_cmd('Input.dispatchMouseEvent', {'type': 'mouseMoved', 'x': x, 'y': y})
                time.sleep(0.08)
                driver.execute_cdp_cmd('Input.dispatchMouseEvent', {'type': 'mousePressed', 'x': x, 'y': y, 'button': 'left', 'clickCount': 1})
                time.sleep(0.08)
                driver.execute_cdp_cmd('Input.dispatchMouseEvent', {'type': 'mouseReleased', 'x': x, 'y': y, 'button': 'left', 'clickCount': 1})
            except Exception:
                try:
                    box.click()
                except Exception:
                    pass
            time.sleep(1.0)
            close_rogue_ad_windows(driver, main_handle)
    except Exception:
        pass

    # 3. Intercept challenge data and fast-solve in Python
    ch_data = None
    for _ in range(15):
        try:
            ch_data = driver.execute_script("return window.__fc_challenge;")
            if ch_data and ch_data.get("challenge"):
                break
        except Exception:
            pass
        time.sleep(0.3)

    if ch_data and ch_data.get("challenge"):
        challenge = ch_data["challenge"]
        difficulty = int(ch_data.get("difficulty", 18))
        try:
            t0 = time.time()
            nonce = fast_solve_sha1_pow(challenge, difficulty)
            t1 = time.time()

            solve_time = t1 - t0
            simulated_wait = random.uniform(14.0, 16.5)
            remaining_wait = max(0.5, simulated_wait - solve_time)
            time.sleep(remaining_wait)

            fake_elapsed_ms = int(simulated_wait * 1000 + random.randint(100, 900))

            driver.execute_script("""
                const w = window.__fc_worker;
                const nonce = arguments[0];
                const ms = arguments[1];
                if (w && w.onmessage) {
                    w.onmessage({
                        data: {
                            type: 'done',
                            nonce: nonce,
                            hashes: nonce + 1,
                            ms: ms,
                            pauses: 0
                        }
                    });
                }
            """, nonce, fake_elapsed_ms)
        except Exception:
            pass

    # 4. Wait for container links to unlock or password prompt to appear
    while time.time() - start < timeout:
        close_rogue_ad_windows(driver, main_handle)

        # Check if password form appeared after captcha
        try_submit_password()

        try:
            # Look for legitimate download links; strictly exclude bot honeypots like /Link/1
            links = driver.find_elements(By.CSS_SELECTOR, "a.button.download, a.btn_dl, a[onclick*='openLink'], button.download, button.btn_dl")
            valid_links = [l for l in links if l.is_displayed() and "display:none" not in (l.get_attribute("style") or "")]
            if valid_links:
                for vl in valid_links:
                    href = vl.get_attribute("href") or ""
                    onclick = vl.get_attribute("onclick") or ""
                    if "openLink(" in onclick:
                        m = re.search(r"openLink\(['\"]([^'\"]+)['\"]\)", onclick)
                        if m and m.group(1) != "1":
                            href = f"https://filecrypt.cc/Link/{m.group(1)}.html"
                    if href and href.startswith("http") and not href.endswith("/Link/1"):
                        try:
                            driver.get(href)
                            time.sleep(2.5)
                            close_rogue_ad_windows(driver, driver.current_window_handle)
                            wait_for_cloudflare(driver, timeout=10)
                            return True
                        except Exception:
                            pass
        except Exception:
            pass

        time.sleep(1.0)

    return False

def extract_direct_download_from_hoster(driver, timeout=15, sniffer=None):
    """Inspects a hoster page (FileDitch, AkiraBox, Vikingfile, Mediafire, etc.) for direct download link."""
    from selenium.webdriver.common.by import By

    start = time.time()
    while time.time() - start < timeout:
        # Un click precedente potrebbe aver avviato un download (bloccato): usa il suo URL
        if sniffer:
            hit = sniffer.poll()
            if hit:
                return hit
        curr_url = driver.current_url or ""

        # 1. Check for specific hoster download links
        try:
            # FileDitch
            if "fileditch" in curr_url or "itsfileditchfiles" in curr_url:
                d_links = driver.find_elements(By.CSS_SELECTOR, "a.btn-main, a.btn")
                for l in d_links:
                    href = l.get_attribute("href") or ""
                    if "fishoilenjoyer.xyz" in href or "download" in l.text.lower():
                        return href

            # AkiraBox
            if "akirabox" in curr_url:
                a_links = driver.find_elements(By.CSS_SELECTOR, "a")
                for l in a_links:
                    href = l.get_attribute("href") or ""
                    if "akirabox.com/download/" in href or "download.akirabox.com" in href:
                        return href

            # Vikingfile
            if "vikingfile" in curr_url or "vik1ngfile" in curr_url:
                try:
                    dl_href = driver.execute_script("return document.getElementById('download-link')?.href || '';")
                    if dl_href and ("/d/" in dl_href or "http" in dl_href):
                        return dl_href
                except Exception:
                    pass
                v_links = driver.find_elements(By.CSS_SELECTOR, "a")
                for l in v_links:
                    href = l.get_attribute("href") or ""
                    if "/d/" in href or "download" in l.text.lower():
                        if "vikingfile.com/d/" in href:
                            return href

            # Mediafire
            if "mediafire.com" in curr_url:
                if "error.php" in curr_url:
                    return None
                mf_btns = driver.find_elements(By.CSS_SELECTOR, "#downloadButton, a.input, a.download_link")
                for btn in mf_btns:
                    href = btn.get_attribute("href") or ""
                    if href.startswith("http") and ("mediafire.com" not in href or "download" in href):
                        return href

            # Pixeldrain
            if "pixeldrain.com" in curr_url:
                pd_links = driver.find_elements(By.CSS_SELECTOR, "a")
                for l in pd_links:
                    href = l.get_attribute("href") or ""
                    if "/api/file/" in href or "download" in l.text.lower():
                        return href

            # 1fichier
            if "1fichier.com" in curr_url:
                of_links = driver.find_elements(By.CSS_SELECTOR, "a.ok.btn-general, a.btn-general.btn-orange, a[href*='1fichier.com/c'], a[href*='a-']")
                for l in of_links:
                    href = l.get_attribute("href") or ""
                    if href.startswith("http") and ("1fichier" in href or "a-" in href or "/c" in href):
                        return href
                f_btns = driver.find_elements(By.CSS_SELECTOR, "button, input[type='submit'], #submit, a.btn")
                for fb in f_btns:
                    txt = (fb.text or fb.get_attribute("value") or "").lower()
                    if any(w in txt for w in ["free download", "télécharger", "telecharger", "access to download", "valider"]):
                        if "⏳" not in txt and fb.is_displayed() and fb.is_enabled():
                            try:
                                fb.click()
                                time.sleep(2.0)
                            except Exception:
                                pass
                            break

            # General download button check
            all_links = driver.find_elements(By.TAG_NAME, "a")
            for l in all_links:
                href = l.get_attribute("href") or ""
                text = (l.text or "").strip().lower()
                if not href.startswith("http"):
                    continue
                if is_direct_stream_url(href):
                    return href
                if any(w in text for w in ["⬇ download", "direct download", "scarica file", "click here to download"]):
                    if not href.endswith(".html") and "abuse" not in href:
                        return href

        except Exception:
            pass

        # Check for countdown timers and click buttons
        try:
            btns = driver.find_elements(By.CSS_SELECTOR, "button, input[type='button'], a.btn")
            for b in btns:
                text = (b.text or b.get_attribute("value") or "").strip().lower()
                if any(kw in text for kw in ["get link", "genera link", "sblocca", "continue", "download now", "free download"]):
                    if b.is_displayed() and b.is_enabled():
                        b.click()
                        time.sleep(1.0)
                        break
        except Exception:
            pass

        time.sleep(1.0)

    return None


INTERACTIVE_BANNER_JS = r"""
(function () {
    if (window.top !== window) return;
    function add() {
        if (!document.body || document.getElementById('__psftp_banner')) return;
        var d = document.createElement('div');
        d.id = '__psftp_banner';
        d.textContent = 'PS FTP Downloader \u2014 completa la verifica e premi \u00abDownload\u00bb: il file verr\u00e0 inviato direttamente alla PlayStation (nessun file salvato sul PC).';
        d.style.cssText = 'position:fixed;left:0;right:0;bottom:0;z-index:2147483647;padding:10px 16px;' +
            'background:linear-gradient(90deg,#1e3a8a,#6d28d9);color:#fff;font:600 13px/1.4 system-ui,sans-serif;' +
            'text-align:center;box-shadow:0 -2px 12px rgba(0,0,0,.4);pointer-events:none;';
        document.body.appendChild(d);
    }
    if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', add); else add();
})();
"""

FILECRYPT_HOOK_JS = """
    window.__fc_challenge = null;
    window.__fc_worker = null;
    const origPost = Worker.prototype.postMessage;
    Worker.prototype.postMessage = function(msg) {
        if (msg && msg.cmd === "start") {
            window.__fc_worker = this;
            window.__fc_challenge = msg;
        }
        return origPost.apply(this, arguments);
    };
"""

def natively_resolvable(url):
    """Pagine hoster che l'app sa risolvere da sola senza browser (Mediafire, 1Fichier)."""
    u = (url or "").lower()
    if "/c" in u and "1fichier.com" in u:
        return False
    return "mediafire.com/file/" in u or "1fichier.com/" in u

def try_fill_password(driver, password):
    from selenium.webdriver.common.by import By
    if not password:
        return
    try:
        pass_inputs = driver.find_elements(By.CSS_SELECTOR, "input[type='password'], input[name*='pass']")
        if pass_inputs and pass_inputs[0].is_displayed():
            pass_inputs[0].clear()
            pass_inputs[0].send_keys(password)
            submits = driver.find_elements(By.CSS_SELECTOR, "input[type='submit'], button[type='submit'], button")
            for sub in submits:
                text = (sub.text or sub.get_attribute("value") or "").lower()
                if any(w in text for w in ["enter", "submit", "unlock", "ok", "invia"]):
                    sub.click()
                    time.sleep(1.5)
                    wait_for_cloudflare(driver, timeout=10)
                    break
    except Exception:
        pass

def detect_page_password(driver, password):
    """Rileva la password mostrata nella pagina (es. Password: [DLPSGAME.COM])."""
    from selenium.webdriver.common.by import By
    detected = password
    try:
        body_text = driver.find_element(By.TAG_NAME, "body").text or ""
        pwd_m = re.search(r"Password\s*:\s*([^\r\n<]+)", body_text, re.IGNORECASE)
        if pwd_m:
            cand = pwd_m.group(1).strip().split("\n")[0].strip()
            if cand:
                detected = cand
    except Exception:
        pass
    return detected

def scan_windows_for_stream(driver):
    for h in driver.window_handles:
        try:
            driver.switch_to.window(h)
            w_url = driver.current_url or ""
            if is_direct_stream_url(w_url) or is_direct_file_url(w_url):
                return w_url
        except Exception:
            pass
    return None

def auto_resolve(driver, url, password, sniffer, headless, deadline):
    from selenium.webdriver.common.by import By

    try:
        driver.get(url)
    except Exception:
        pass

    hit = sniffer.poll()
    if hit:
        return success_result(driver, hit, "captured_download", password)

    wait_for_cloudflare(driver, timeout=20)
    try_fill_password(driver, password)
    detected_password = detect_page_password(driver, password)

    # Step 1: If on protector page (e.g. downloadgameps3.net, filecrypt), collect hoster links
    current_page = driver.current_url or url
    if is_protector_url(current_page):
        if "filecrypt." in current_page:
            handle_filecrypt(driver, timeout=35, password=password)
            best_win = None
            best_score = 0
            for h in driver.window_handles:
                try:
                    driver.switch_to.window(h)
                    w_url = (driver.current_url or "").lower()
                    score = 0
                    for hd in HOSTER_DOMAINS:
                        if hd in w_url:
                            score = 10
                            if any(part in w_url for part in ["/file/", "/d/", "/download", ".pkg", ".rar", ".7z", ".zip"]):
                                score = 100
                            elif w_url.rstrip("/").count("/") > 2:
                                score = 50
                            break
                    if is_direct_file_url(w_url):
                        score = 150
                    if score > best_score:
                        best_score = score
                        best_win = h
                except Exception:
                    pass
            if best_win and best_score > 0:
                driver.switch_to.window(best_win)
                current_page = driver.current_url or ""

            hit = sniffer.poll()
            if hit:
                return success_result(driver, hit, "captured_download", detected_password)

        if is_protector_url(current_page):
            hoster_candidates = []
            links = driver.find_elements(By.TAG_NAME, "a")
            for l in links:
                try:
                    href = l.get_attribute("href") or ""
                    onclick = l.get_attribute("onclick") or ""
                    text = (l.text or "").strip()
                    style = l.get_attribute("style") or ""
                except Exception:
                    continue
                if "display:none" in style:
                    continue

                if "openLink(" in onclick:
                    m = re.search(r"openLink\(['\"]([^'\"]+)['\"]\)", onclick)
                    if m:
                        href = f"https://filecrypt.cc/Link/{m.group(1)}.html"

                if not href.startswith("http") or is_protector_url(href):
                    continue
                # Priority: Mediafire, FileDitch, Pixeldrain, Vikingfile, Rootz, AkiraBox, 1fichier
                prio = 99
                if "mediafire" in href: prio = 1
                elif "itsfileditchfiles" in href or "fileditch" in href: prio = 2
                elif "pixeldrain" in href: prio = 3
                elif "vikingfile" in href or "vik1ngfile" in href: prio = 4
                elif "datanodes" in href or "rootz.so" in href: prio = 5
                elif "akirabox" in href: prio = 6
                elif "1fichier" in href: prio = 7

                if prio < 99 or any(h in href for h in HOSTER_DOMAINS):
                    hoster_candidates.append((prio, href, text))

            if hoster_candidates:
                hoster_candidates.sort(key=lambda x: x[0])
                for _, cand_url, _ in hoster_candidates:
                    if time.time() > deadline:
                        break
                    try:
                        driver.get(cand_url)
                        hit = sniffer.poll()
                        if hit:
                            return success_result(driver, hit, "captured_download", detected_password)
                        wait_for_cloudflare(driver, timeout=12)
                        cand_direct = extract_direct_download_from_hoster(driver, timeout=8, sniffer=sniffer)
                        if cand_direct and cand_direct == sniffer.found:
                            return success_result(driver, cand_direct, "captured_download", detected_password)
                        if not cand_direct:
                            curr = driver.current_url or ""
                            if any(err_ind in curr.lower() for err_ind in ["error.php", "errno=", "deleted", "notfound", "file_removed"]):
                                continue
                            if is_direct_stream_url(curr):
                                cand_direct = curr
                        if not cand_direct:
                            cand_direct = scan_windows_for_stream(driver)
                        if cand_direct:
                            cookie_str, user_agent, referer_url = collect_session(driver, cand_direct)
                            if verify_download_link(cand_direct, cookie_str, user_agent, referer_url or cand_url):
                                return success_result(driver, cand_direct, "resolved_direct_cdn", detected_password)
                        # Mediafire / 1Fichier: l'app ha risolutori nativi, restituisce la pagina hoster
                        if natively_resolvable(cand_url):
                            return success_result(driver, cand_url, "hoster_page", detected_password)
                    except Exception:
                        continue

                return {
                    "success": False,
                    "error": "ALL_HOSTERS_OFFLINE",
                    "message": "Tutti i mirror per questo file risultano rimossi o non disponibili (404/403/scaduti)."
                }

    # Step 2: Now on hoster page (or already on hoster) — extract direct download
    direct_download_url = extract_direct_download_from_hoster(driver, timeout=15, sniffer=sniffer)
    if direct_download_url and direct_download_url == sniffer.found:
        return success_result(driver, direct_download_url, "captured_download", detected_password)

    # If not found via direct extract, check if current URL redirected to a stream
    if not direct_download_url:
        curr = driver.current_url or ""
        if is_direct_stream_url(curr):
            direct_download_url = curr

    if not direct_download_url:
        # Fallback: check all open windows / popups
        try:
            direct_download_url = scan_windows_for_stream(driver)
        except Exception:
            pass

    hit = sniffer.poll()
    if hit:
        return success_result(driver, hit, "captured_download", detected_password)

    if direct_download_url:
        cookie_str, user_agent, referer_url = collect_session(driver, direct_download_url)
        if verify_download_link(direct_download_url, cookie_str, user_agent, referer_url or current_page):
            return success_result(driver, direct_download_url, "resolved_direct_cdn", detected_password)

    final_url = driver.current_url or ""
    if final_url and final_url != url:
        if any(err_ind in final_url.lower() for err_ind in ["error.php", "errno=", "deleted", "notfound", "file_removed", "removed"]):
            return {
                "success": False,
                "error": "FILE_REMOVED",
                "message": "Il file è stato rimosso o eliminato dall'hoster (DMCA/link scaduto)."
            }
        if is_direct_stream_url(final_url) or is_direct_file_url(final_url):
            return success_result(driver, final_url, "resolved_direct_cdn", detected_password)
        if natively_resolvable(final_url):
            return success_result(driver, final_url, "hoster_page", detected_password)

    if "filecrypt." in current_page:
        if headless:
            return {
                "success": False,
                "error": "CAPTCHA_INTERACTIVE",
                "message": "FileCrypt richiede una verifica CutCaptcha da completare manualmente."
            }
        return {
            "success": False,
            "error": "CAPTCHA_TIMEOUT",
            "message": "FileCrypt: verifica umana non completata in tempo."
        }

    return {
        "success": False,
        "error": "TIMEOUT",
        "message": "Impossibile trovare il link di download diretto sulla pagina."
    }

def interactive_resolve(driver, url, password, sniffer, deadline):
    """Finestra visibile in cui l'utente completa captcha/attese: ogni download avviato
    viene bloccato nel browser e il suo URL restituito all'app."""
    try:
        driver.get(url)
    except Exception:
        pass

    hit = sniffer.poll()
    if hit:
        return success_result(driver, hit, "captured_download", password)

    wait_for_cloudflare(driver, timeout=15)
    try_fill_password(driver, password)
    detected_password = detect_page_password(driver, password)

    try:
        if "filecrypt." in (driver.current_url or ""):
            handle_filecrypt(driver, timeout=25, password=password)
    except Exception:
        pass

    last_scan = 0.0
    while time.time() < deadline:
        hit = sniffer.poll()
        if hit:
            return success_result(driver, hit, "captured_download", detected_password)

        try:
            handles = driver.window_handles
        except Exception:
            handles = []
        if not handles:
            return {
                "success": False,
                "error": "USER_CLOSED",
                "message": "Finestra di sblocco chiusa prima di avviare il download."
            }

        if time.time() - last_scan > 2.0:
            last_scan = time.time()
            try:
                try:
                    cur = driver.current_url or ""
                except Exception:
                    driver.switch_to.window(handles[-1])
                    cur = driver.current_url or ""
                if is_direct_stream_url(cur) or is_direct_file_url(cur):
                    return success_result(driver, cur, "resolved_direct_cdn", detected_password)
                hrefs = driver.execute_script(
                    "return Array.from(document.querySelectorAll('a[href]')).slice(0, 800).map(a => a.href);"
                ) or []
                for h in hrefs:
                    if isinstance(h, str) and is_direct_stream_url(h):
                        cookie_str, user_agent, referer_url = collect_session(driver, h)
                        if verify_download_link(h, cookie_str, user_agent, referer_url):
                            return success_result(driver, h, "resolved_direct_cdn", detected_password)
            except Exception:
                pass

        time.sleep(0.5)

    return {
        "success": False,
        "error": "USER_TIMEOUT",
        "message": "Tempo scaduto: nessun download avviato nella finestra di sblocco."
    }

def solve_url(url, password=None, timeout=45, headless=False, custom_browser=None, interactive=False):
    deps = check_dependencies()
    if not deps["ready"]:
        return {
            "success": False,
            "error": "MISSING_DEPS",
            "message": "undetected-chromedriver o browser Chromium/Chrome non trovato",
            "details": deps
        }

    if interactive:
        headless = False
    deadline = time.time() + max(20, timeout)
    browser_bin = custom_browser or deps["browser"]

    import undetected_chromedriver as uc

    temp_profile_dir = tempfile.mkdtemp(prefix="ps_uc_")
    trash_dir = os.path.join(temp_profile_dir, "blocked-downloads")

    options = uc.ChromeOptions()
    options.add_argument("--no-sandbox")
    options.add_argument("--disable-dev-shm-usage")
    options.add_argument("--disable-gpu")
    options.add_argument("--disable-blink-features=AutomationControlled")
    # Performance log: necessario per intercettare l'URL dei download bloccati
    options.set_capability("goog:loggingPrefs", {"performance": "ALL"})
    prefs = {
        # Rete di sicurezza: anche se un download sfuggisse al blocco CDP finirebbe nel
        # profilo temporaneo (eliminato a fine sessione) e mai in ~/Download.
        # Nota: \"/dev/null\" NON funziona (Chromium ripiega su ~/Download).
        "download.default_directory": trash_dir,
        "download.prompt_for_download": False,
        "download.directory_upgrade": True,
        "download_restrictions": 3,
        "safebrowsing.enabled": False,
        "plugins.always_open_pdf_externally": False,
        "profile.default_content_setting_values.automatic_downloads": 2,
        "profile.default_content_setting_values.popups": 2,
        "profile.default_content_setting_values.notifications": 2,
    }
    options.add_experimental_option("prefs", prefs)
    if headless:
        # Off-screen window avoids Cloudflare Turnstile's headless bot detection
        # while keeping the browser completely invisible to the user desktop
        options.add_argument("--window-position=-10000,-10000")
        options.add_argument("--window-size=1280,800")
    else:
        options.add_argument("--window-size=1180,820")

    driver = None
    try:
        driver = uc.Chrome(
            options=options,
            user_data_dir=temp_profile_dir,
            browser_executable_path=browser_bin,
            use_subprocess=True
        )
        block_browser_downloads(driver)
        sniffer = DownloadSniffer(driver)

        scripts = [FILECRYPT_HOOK_JS]
        if interactive:
            scripts.append(INTERACTIVE_BANNER_JS)
        for src in scripts:
            try:
                driver.execute_cdp_cmd("Page.addScriptToEvaluateOnNewDocument", {"source": src})
            except Exception:
                pass

        driver.set_page_load_timeout(30)
        if interactive:
            return interactive_resolve(driver, url, password, sniffer, deadline)
        return auto_resolve(driver, url, password, sniffer, headless, deadline)

    except Exception as e:
        return {
            "success": False,
            "error": "EXCEPTION",
            "message": str(e)
        }
    finally:
        if driver:
            try:
                driver.quit()
            except Exception:
                pass
        if temp_profile_dir and os.path.exists(temp_profile_dir):
            shutil.rmtree(temp_profile_dir, ignore_errors=True)

class ResolverTimeout(BaseException):
    """BaseException: non viene intercettata dai numerosi `except Exception` interni."""

def _on_alarm(signum, frame):
    raise ResolverTimeout()

def main():
    parser = argparse.ArgumentParser(description="PS-FTP-Downloader UC Bot Resolver")
    parser.add_argument("url", nargs="?", help="Target protected URL to resolve")
    parser.add_argument("--password", "-p", help="Optional password for protected links")
    parser.add_argument("--timeout", "-t", type=int, default=90, help="Timeout in seconds")
    parser.add_argument("--headless", action="store_true", help="Run browser in headless mode")
    parser.add_argument("--interactive", action="store_true",
                        help="Visible window: the user unlocks the link, downloads are intercepted")
    parser.add_argument("--browser", help="Path to browser executable")
    parser.add_argument("--check", action="store_true", help="Check dependencies and exit")

    args = parser.parse_args()

    if args.check:
        status = check_dependencies()
        print(json.dumps(status))
        sys.exit(0 if status["ready"] else 1)

    if not args.url:
        parser.print_help()
        sys.exit(1)

    # Limite rigido: il processo non deve mai restare appeso (l'app attende l'output)
    signal.signal(signal.SIGALRM, _on_alarm)
    signal.alarm(max(30, args.timeout + 20))
    try:
        res = solve_url(
            url=args.url,
            password=args.password,
            timeout=args.timeout,
            headless=args.headless,
            custom_browser=args.browser,
            interactive=args.interactive
        )
    except ResolverTimeout:
        res = {
            "success": False,
            "error": "TIMEOUT",
            "message": "Tempo massimo di risoluzione superato."
        }
    finally:
        signal.alarm(0)

    print(json.dumps(res))
    sys.stdout.flush()
    sys.exit(0 if res.get("success") else 1)

if __name__ == "__main__":
    main()
