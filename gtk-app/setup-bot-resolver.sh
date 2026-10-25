#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────
#  setup-bot-resolver.sh — Configure Undetected ChromeDriver
#                          for PS FTP Downloader
# ─────────────────────────────────────────────────────────────────

set -euo pipefail

BOLD="\033[1m"; GREEN="\033[1;32m"; CYAN="\033[1;36m"; YELLOW="\033[1;33m"; RED="\033[1;31m"; RESET="\033[0m"

echo -e "${CYAN}"
echo "  ========================================================"
echo "    PS FTP Downloader — Undetected ChromeDriver Setup     "
echo "  ========================================================"
echo -e "${RESET}"

# 1. Check Python 3
if ! command -v python3 &>/dev/null; then
    echo -e "${RED}[ERROR] python3 non trovato.${RESET} Installa python3 con: sudo apt install python3"
    exit 1
fi
echo -e "  ${GREEN}[OK]${RESET} Python3 rilevato: $(python3 --version)"

# 2. Check Chrome / Chromium browser
CHROME_FOUND=""
for b in /snap/bin/chromium /usr/bin/google-chrome-stable /usr/bin/google-chrome /usr/bin/chromium-browser /usr/bin/chromium; do
    if [ -x "$b" ]; then
        CHROME_FOUND="$b"
        break
    fi
done

if [ -z "$CHROME_FOUND" ]; then
    echo -e "${YELLOW}[WARN] Nessun browser Chromium o Google Chrome trovato.${RESET}"
    echo "  Consigliato: installa Chromium con 'sudo snap install chromium' o Google Chrome."
else
    echo -e "  ${GREEN}[OK]${RESET} Browser compatibile trovato: $CHROME_FOUND"
fi

# 3. Check / Install pip
if ! python3 -m pip --version &>/dev/null; then
    echo -e "${YELLOW}[INFO] Installazione pip utente in ~/.local/bin...${RESET}"
    curl -sS https://bootstrap.pypa.io/get-pip.py | python3 - --user --break-system-packages
fi

# Ensure ~/.local/bin is in PATH for this run
export PATH="$HOME/.local/bin:$PATH"

# 4. Install undetected-chromedriver & selenium
echo -e "${CYAN}[INFO] Installazione/Aggiornamento undetected-chromedriver e selenium...${RESET}"
python3 -m pip install --user --break-system-packages --upgrade undetected-chromedriver selenium

# 5. Verify setup
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RESOLVER="$SCRIPT_DIR/uc_resolver.py"
if [ ! -f "$RESOLVER" ]; then
    RESOLVER="/usr/share/ps-ftp-downloader/uc_resolver.py"
fi

if [ -f "$RESOLVER" ]; then
    chmod +x "$RESOLVER"
    echo -e "${CYAN}[INFO] Test del modulo resolver...${RESET}"
    if python3 "$RESOLVER" --check; then
        echo -e "${GREEN}${BOLD}  Undetected ChromeDriver è configurato e pronto all'uso!${RESET}"
    else
        echo -e "${RED}[ERRORE] Il test del resolver non è andato a buon fine.${RESET}"
        exit 1
    fi
else
    echo -e "${GREEN}${BOLD}  Librerie Python installate con successo!${RESET}"
fi
