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
    "itsfileditchfiles.st",
    "fileditch.com",
    "akirabox.to",
    "akirabox.com",
    "vikingfile.com",
    "vik1ngfile.site",
    "rootz.so",
    "datanodes.to",
    "mediafire.com",
    "1fichier.com",
    "pixeldrain.com",
    "mega.nz",
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
    netloc = urlparse(url).netloc.lower()
    return any(p in netloc for p in PROTECTOR_DOMAINS)

def is_hoster_landing_page(url):
    netloc = urlparse(url).netloc.lower()
    return any(h in netloc for h in HOSTER_DOMAINS)

def is_direct_stream_url(url):
    for p in DIRECT_CDN_PATTERNS:
        if p.search(url):
            return True
    return False

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
    from selenium.webdriver.common.action_chains import ActionChains

    start = time.time()

    # 1. Handle password form if present
    if password:
        try:
            pass_inps = driver.find_elements(By.CSS_SELECTOR, "#p4assw0rt, input[name='password']")
            if pass_inps and pass_inps[0].is_displayed():
                pass_inps[0].clear()
                pass_inps[0].send_keys(password)
                submits = driver.find_elements(By.CSS_SELECTOR, "button, input[type='submit']")
                for s in submits:
                    if s.is_displayed():
                        s.click()
                        time.sleep(2.0)
                        break
        except Exception:
            pass

    # 2. Click CutCaptcha / PoW checkbox to initiate
    try:
        boxes = driver.find_elements(By.CSS_SELECTOR, ".pow-captcha__box, #pow-captcha")
        if boxes and boxes[0].is_displayed():
            try:
                actions = ActionChains(driver)
                actions.move_to_element(boxes[0]).pause(0.3).click().perform()
            except Exception:
                try:
                    boxes[0].click()
                except Exception:
                    pass
    except Exception:
        pass

    # 3. Intercept challenge data and fast-solve in Python
    ch_data = None
    for _ in range(25):
        try:
            ch_data = driver.execute_script("return window.__fc_challenge;")
            if ch_data and ch_data.get("challenge"):
                break
        except Exception:
            pass
        time.sleep(0.3)

    if ch_data and ch_data.get("challenge"):
        challenge = ch_data["challenge"]
        difficulty = int(ch_data.get("difficulty", 24))
        try:
            t0 = time.time()
            nonce = fast_solve_sha1_pow(challenge, difficulty)
            t1 = time.time()

            # Wait realistic time (14-16s) so FileCrypt biometrics & timing validation pass
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
                            type: "done",
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

    # 4. Wait for container links to unlock and navigate to hoster
    while time.time() - start < timeout:
        try:
            links = driver.find_elements(By.CSS_SELECTOR, "a.button.download, a.btn_dl, a[onclick*='openLink'], a[href*='/Link/']")
            valid_links = [l for l in links if l.is_displayed() and "display:none" not in (l.get_attribute("style") or "")]
            if valid_links:
                href = valid_links[0].get_attribute("href") or ""
                onclick = valid_links[0].get_attribute("onclick") or ""
                if "openLink(" in onclick:
                    m = re.search(r"openLink\(['\"]([^'\"]+)['\"]\)", onclick)
                    if m:
                        href = f"https://filecrypt.cc/Link/{m.group(1)}.html"
                if href and href.startswith("http"):
                    try:
                        driver.get(href)
                        time.sleep(3.0)
                        wait_for_cloudflare(driver, timeout=10)
                        return True
                    except Exception:
                        pass
                # Fallback: click link
                try:
                    valid_links[0].click()
                    time.sleep(3.0)
                    return True
                except Exception:
                    return True
        except Exception:
            pass

        time.sleep(1.0)

    return False

def extract_direct_download_from_hoster(driver, timeout=15):
    """Inspects a hoster page (FileDitch, AkiraBox, Vikingfile, Mediafire, etc.) for direct download link."""
    from selenium.webdriver.common.by import By

    start = time.time()
    while time.time() - start < timeout:
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
                    if href.startswith("http") and "mediafire.com" not in href or "download" in href:
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
                of_links = driver.find_elements(By.CSS_SELECTOR, "a.ok.btn-general, a.btn-general.btn-orange, a[href*='1fichier.com/c/'], a[href*='a-']")
                for l in of_links:
                    href = l.get_attribute("href") or ""
                    if href.startswith("http") and ("1fichier" in href or "a-" in href or "/c/" in href):
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

def solve_url(url, password=None, timeout=45, headless=False, custom_browser=None):
    deps = check_dependencies()
    if not deps["ready"]:
        return {
            "success": False,
            "error": "MISSING_DEPS",
            "message": "undetected-chromedriver o browser Chromium/Chrome non trovato",
            "details": deps
        }

    browser_bin = custom_browser or deps["browser"]

    import undetected_chromedriver as uc
    from selenium.webdriver.common.by import By

    import tempfile
    temp_profile_dir = tempfile.mkdtemp(prefix="ps_uc_")

    options = uc.ChromeOptions()
    options.add_argument("--no-sandbox")
    options.add_argument("--disable-dev-shm-usage")
    options.add_argument("--disable-gpu")
    options.add_argument("--disable-popup-blocking")
    options.add_argument("--disable-blink-features=AutomationControlled")
    prefs = {
        "download.default_directory": "/dev/null",
        "download.prompt_for_download": False,
        "download.directory_upgrade": True,
        "safebrowsing.enabled": False,
        "plugins.always_open_pdf_externally": True,
    }
    options.add_experimental_option("prefs", prefs)
    if headless:
        # Off-screen window avoids Cloudflare Turnstile's headless bot detection
        # while keeping the browser completely invisible to the user desktop
        options.add_argument("--window-position=-10000,-10000")
        options.add_argument("--window-size=1280,800")
    else:
        options.add_argument("--window-size=1100,750")

    driver = None
    try:
        driver = uc.Chrome(
            options=options,
            user_data_dir=temp_profile_dir,
            browser_executable_path=browser_bin,
            use_subprocess=True
        )
        try:
            driver.execute_cdp_cmd("Page.addScriptToEvaluateOnNewDocument", {
                "source": """
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
            })
        except Exception:
            pass

        driver.set_page_load_timeout(30)
        try:
            driver.get(url)
        except Exception:
            pass

        start_time = time.time()
        wait_for_cloudflare(driver, timeout=20)

        # Handle password submission if protected
        if password:
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

        # Detect password displayed on page (e.g. Password: [DLPSGAME.COM])
        detected_password = password
        try:
            body_text = driver.find_element(By.TAG_NAME, "body").text or ""
            pwd_m = re.search(r"Password\s*:\s*([^\r\n<]+)", body_text, re.IGNORECASE)
            if pwd_m:
                cand = pwd_m.group(1).strip().split("\n")[0].strip()
                if cand:
                    detected_password = cand
        except Exception:
            pass

        # Step 1: If on protector page (e.g. downloadgameps3.net, filecrypt), collect hoster links
        current_page = driver.current_url or url
        if is_protector_url(current_page):
            if "filecrypt." in current_page:
                handle_filecrypt(driver, timeout=35, password=password)
                best_win = None
                best_score = -1
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
                        if score > best_score:
                            best_score = score
                            best_win = h
                    except Exception:
                        pass
                if best_win:
                    driver.switch_to.window(best_win)
                    current_page = driver.current_url or ""

            if is_protector_url(current_page):
                hoster_candidates = []
                links = driver.find_elements(By.TAG_NAME, "a")
                for l in links:
                    href = l.get_attribute("href") or ""
                    onclick = l.get_attribute("onclick") or ""
                    text = (l.text or "").strip()
                    style = l.get_attribute("style") or ""
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
                        try:
                            driver.get(cand_url)
                            wait_for_cloudflare(driver, timeout=12)
                            cand_direct = extract_direct_download_from_hoster(driver, timeout=8)
                            if not cand_direct:
                                curr = driver.current_url or ""
                                if is_direct_stream_url(curr):
                                    cand_direct = curr
                            if not cand_direct:
                                for h in driver.window_handles:
                                    try:
                                        driver.switch_to.window(h)
                                        w_url = driver.current_url or ""
                                        if is_direct_stream_url(w_url):
                                            cand_direct = w_url
                                            break
                                    except Exception:
                                        pass
                            if cand_direct:
                                cookie_parts = [f"{c['name']}={c['value']}" for c in driver.get_cookies()]
                                cookie_str = "; ".join(cookie_parts)
                                user_agent = driver.execute_script("return navigator.userAgent;") or ""
                                referer_url = driver.current_url or cand_url
                                if verify_download_link(cand_direct, cookie_str, user_agent, referer_url):
                                    return {
                                        "success": True,
                                        "url": cand_direct,
                                        "title": driver.title,
                                        "cookie": cookie_str,
                                        "user_agent": user_agent,
                                        "referer": referer_url,
                                        "password": detected_password or "",
                                        "source": "resolved_direct_cdn"
                                    }
                        except Exception:
                            continue

                    return {
                        "success": False,
                        "error": "ALL_HOSTERS_OFFLINE",
                        "message": "Tutti i mirror per questo file risultano rimossi o non disponibili (404/403/scaduti)."
                    }

        # Step 2: Now on hoster page (or already on hoster) — extract direct download
        direct_download_url = extract_direct_download_from_hoster(driver, timeout=15)

        # If not found via direct extract, check if current URL redirected to a stream
        if not direct_download_url:
            curr = driver.current_url or ""
            if is_direct_stream_url(curr):
                direct_download_url = curr

        if not direct_download_url:
            # Fallback: check all open windows / popups
            try:
                for h in driver.window_handles:
                    driver.switch_to.window(h)
                    w_url = driver.current_url or ""
                    if is_direct_stream_url(w_url):
                        direct_download_url = w_url
                        break
            except Exception:
                pass

        # Extract browser session cookies & user agent
        cookie_parts = []
        try:
            for c in driver.get_cookies():
                cookie_parts.append(f"{c['name']}={c['value']}")
        except Exception:
            pass
        cookie_str = "; ".join(cookie_parts)

        user_agent = ""
        try:
            user_agent = driver.execute_script("return navigator.userAgent;")
        except Exception:
            pass

        referer_url = driver.current_url or current_page
        if direct_download_url and verify_download_link(direct_download_url, cookie_str, user_agent, referer_url):
            return {
                "success": True,
                "url": direct_download_url,
                "title": driver.title,
                "cookie": cookie_str,
                "user_agent": user_agent,
                "referer": referer_url,
                "password": detected_password or "",
                "source": "resolved_direct_cdn"
            }

        # If still on a hoster landing page and could not extract direct, return hoster page URL with cookies
        final_url = driver.current_url
        if final_url and final_url != url:
            return {
                "success": True,
                "url": final_url,
                "title": driver.title,
                "cookie": cookie_str,
                "user_agent": user_agent,
                "referer": final_url,
                "password": detected_password or "",
                "source": "hoster_page"
            }

        if "filecrypt." in current_page:
            if headless:
                return {
                    "success": False,
                    "error": "CAPTCHA_INTERACTIVE",
                    "message": "FileCrypt richiede verifica CutCaptcha: disabilita 'Modalità Headless' nelle impostazioni per completare il captcha nel browser."
                }
            else:
                return {
                    "success": False,
                    "error": "CAPTCHA_TIMEOUT",
                    "message": "FileCrypt: verifica umana non completata in tempo. Riprova aprendo il link nel browser."
                }

        return {
            "success": False,
            "error": "TIMEOUT",
            "message": "Impossibile trovare il link di download diretto sulla pagina."
        }

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
        if 'temp_profile_dir' in locals() and temp_profile_dir and os.path.exists(temp_profile_dir):
            try:
                shutil.rmtree(temp_profile_dir, ignore_errors=True)
            except Exception:
                pass

def main():
    parser = argparse.ArgumentParser(description="PS-FTP-Downloader UC Bot Resolver")
    parser.add_argument("url", nargs="?", help="Target protected URL to resolve")
    parser.add_argument("--password", "-p", help="Optional password for protected links")
    parser.add_argument("--timeout", "-t", type=int, default=45, help="Timeout in seconds")
    parser.add_argument("--headless", action="store_true", help="Run browser in headless mode")
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

    res = solve_url(
        url=args.url,
        password=args.password,
        timeout=args.timeout,
        headless=args.headless,
        custom_browser=args.browser
    )

    print(json.dumps(res))
    sys.exit(0 if res.get("success") else 1)

if __name__ == "__main__":
    main()
