#!/usr/bin/env bash

# ─────────────────────────────────────────────────────────────────
#  PS FTP Downloader — CLI
#  Download PKG and files directly from the network -> PS via FTP
#  without saving anything locally.
#
#  Uses PS FTP in HEN / Jailbreak mode
#  (GoldHen, ps4-hen, etc.)
#
#  Dependencies: wget, curl, pv
#  Install: sudo apt install wget curl pv
# ─────────────────────────────────────────────────────────────────

FTP_HOST="192.168.1.111"
FTP_PORT="2121"
FTP_DIR="data/pkg"

BOLD="\033[1m"
CYAN="\033[1;36m"
GREEN="\033[1;32m"
YELLOW="\033[1;33m"
RED="\033[1;31m"
BLUE="\033[1;34m"
DIM="\033[2m"
RESET="\033[0m"

clear
echo -e "${CYAN}"
echo "  ============================================"
echo "           PS FTP Downloader  v1.0            "
echo "    Network -> PS FTP (No local storage)      "
echo "  ============================================"
echo -e "${RESET}"
echo -e "  ${DIM}Target: ftp://${FTP_HOST}:${FTP_PORT}/${FTP_DIR}/${RESET}"
echo ""

# Check dependencies
MISSING=()
for cmd in wget curl pv; do
    command -v "$cmd" &>/dev/null || MISSING+=("$cmd")
done

if [ ${#MISSING[@]} -gt 0 ]; then
    echo -e "${RED}[Error] Missing tools: ${MISSING[*]}${RESET}"
    echo -e "${YELLOW}  Install with: sudo apt install ${MISSING[*]}${RESET}"
    exit 1
fi

echo -e "${BOLD}File URL to download to PS:${RESET}"
read -rp "  -> " DOWNLOAD_URL

if [[ -z "$DOWNLOAD_URL" ]]; then
    echo -e "${RED}[Error] No URL entered. Exiting.${RESET}"
    exit 1
fi

echo ""
echo -e "${DIM}  Retrieving file information...${RESET}"

HEADER=$(curl -sI --max-time 10 -L "$DOWNLOAD_URL" 2>/dev/null)
FILE_SIZE=$(echo "$HEADER" | grep -i "Content-Length" | tail -1 | awk '{print $2}' | tr -d '\r')
REMOTE_FILENAME=$(echo "$HEADER" | grep -i "Content-Disposition" \
    | grep -oP 'filename="?\K[^";\r]+' | head -1)

if [[ -z "$REMOTE_FILENAME" ]]; then
    REMOTE_FILENAME=$(basename "${DOWNLOAD_URL%%\?*}" | sed 's/#.*//')
fi

EXT="${REMOTE_FILENAME##*.}"
if [[ "$EXT" == "$REMOTE_FILENAME" || -z "$EXT" || ${#EXT} -gt 10 ]]; then
    EXT=""
fi

echo -e "  [OK] Detected file: ${BOLD}${REMOTE_FILENAME}${RESET}"

if [[ -n "$FILE_SIZE" && "$FILE_SIZE" -gt 0 ]]; then
    if   (( FILE_SIZE >= 1073741824 )); then
        SIZE_HR=$(awk "BEGIN {printf \"%.2f GB\", $FILE_SIZE/1073741824}")
    elif (( FILE_SIZE >= 1048576 )); then
        SIZE_HR=$(awk "BEGIN {printf \"%.2f MB\", $FILE_SIZE/1048576}")
    else
        SIZE_HR="${FILE_SIZE} B"
    fi
    echo -e "  [OK] Size: ${BOLD}${SIZE_HR}${RESET}"
else
    FILE_SIZE=""
    echo -e "  [Warn] Size unavailable"
fi

echo ""

if [[ -n "$EXT" ]]; then
    echo -e "${BOLD}Destination filename on PS${RESET} ${DIM}(without extension, .${EXT} will be added):${RESET}"
else
    echo -e "${BOLD}Destination filename on PS${RESET} ${DIM}(include extension if necessary):${RESET}"
fi

read -rp "  -> " BASE_NAME

if [[ -z "$BASE_NAME" ]]; then
    echo -e "${RED}[Error] Filename not entered. Exiting.${RESET}"
    exit 1
fi

if [[ -n "$EXT" ]]; then
    DEST_FILENAME="${BASE_NAME}.${EXT}"
else
    DEST_FILENAME="${BASE_NAME}"
fi

FTP_URL="ftp://${FTP_HOST}:${FTP_PORT}/${FTP_DIR}/${DEST_FILENAME}"

echo ""
echo -e "${CYAN}---------------------------------------------${RESET}"
echo -e "  ${BOLD}Transfer Summary${RESET}"
echo -e "  ${DIM}Source    :${RESET} ${DOWNLOAD_URL}"
echo -e "  ${DIM}PS (FTP)  :${RESET} ${FTP_URL}"
echo -e "${CYAN}---------------------------------------------${RESET}"
echo ""
read -rp "  Confirm and start transfer to PS? [Y/n] " CONFIRM
CONFIRM="${CONFIRM,,}"
if [[ "$CONFIRM" == "n" || "$CONFIRM" == "no" ]]; then
    echo -e "${YELLOW}  Operation cancelled.${RESET}"
    exit 0
fi

echo ""
echo -e "${BLUE}  Streaming to PS...${RESET}"
echo ""

if [[ -n "$FILE_SIZE" ]]; then
    wget -q -O - "$DOWNLOAD_URL" 2>/dev/null \
        | pv -s "$FILE_SIZE" -petrab \
        | curl --silent --show-error -T - "${FTP_URL}"
else
    wget -q -O - "$DOWNLOAD_URL" 2>/dev/null \
        | pv -petrab \
        | curl --silent --show-error -T - "${FTP_URL}"
fi

EXIT_CODE=$?

echo ""
if [[ $EXIT_CODE -eq 0 ]]; then
    echo -e "${GREEN}${BOLD}  [OK] File successfully sent to PS!${RESET}"
    echo -e "  ${DIM}Available on PS at: /${FTP_DIR}/${DEST_FILENAME}${RESET}"
else
    echo -e "${RED}${BOLD}  [Error] Transfer failed (code: ${EXIT_CODE})${RESET}"
    echo -e "  ${YELLOW}  - Verify PS is on and in HEN/GoldHen/FTP mode${RESET}"
    echo -e "  ${YELLOW}  - Check IP address: ${FTP_HOST}:${FTP_PORT}${RESET}"
    exit $EXIT_CODE
fi

echo ""
