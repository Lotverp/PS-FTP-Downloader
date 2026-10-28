#!/usr/bin/env bash

# ─────────────────────────────────────────────────────────────────
#  P2C Downloader — CLI
#  Scarica file e PKG direttamente dalla rete alla PlayStation via FTP
#  senza salvare nulla localmente su disco.
#
#  Supporta console PS con server FTP attivo (GoldHEN, ps4-hen, ecc.)
#
#  Dipendenze: wget, curl, pv
#  Installazione: sudo apt install wget curl pv
# ─────────────────────────────────────────────────────────────────

FTP_HOST="${FTP_HOST:-192.168.1.195}"
FTP_PORT="${FTP_PORT:-2121}"
FTP_DIR="${FTP_DIR:-data/pkg}"
FTP_DIR="${FTP_DIR#/}"
FTP_DIR="${FTP_DIR%/}"

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
echo "           P2C Downloader  v1.3.5             "
echo "    Rete -> Console FTP (Nessuno storage locale) "
echo "  ============================================"
echo -e "${RESET}"
echo -e "  ${DIM}Destinazione: ftp://${FTP_HOST}:${FTP_PORT}/${FTP_DIR}/${RESET}"
echo ""

# Verifica dipendenze di sistema
MISSING=()
for cmd in wget curl pv; do
    command -v "$cmd" &>/dev/null || MISSING+=("$cmd")
done

if [ ${#MISSING[@]} -gt 0 ]; then
    echo -e "${RED}[Errore] Strumenti mancanti: ${MISSING[*]}${RESET}"
    echo -e "${YELLOW}  Installa con: sudo apt install ${MISSING[*]}${RESET}"
    exit 1
fi

# Assicura automaticamente che la cartella FTP remota esista sulla PS
echo -e "${DIM}  Connessione a Console FTP (ftp://${FTP_HOST}:${FTP_PORT}/${FTP_DIR}/)...${RESET}"
if curl -s --connect-timeout 4 -m 8 -u "anonymous:anonymous@" --ftp-create-dirs "ftp://${FTP_HOST}:${FTP_PORT}/${FTP_DIR}/" -o /dev/null; then
    echo -e "  ${GREEN}[OK] Connesso alla PS — Cartella '${FTP_DIR}' verificata e pronta!${RESET}"
else
    echo -e "  ${YELLOW}[Avviso] Impossibile verificare/creare ftp://${FTP_HOST}:${FTP_PORT}/${FTP_DIR}/${RESET}"
    echo -e "  ${DIM}  (Assicurati che GoldHEN / server FTP sia abilitato sulla console)${RESET}"
fi
echo ""

echo -e "${BOLD}URL del file da scaricare sulla PS:${RESET}"
read -rp "  -> " DOWNLOAD_URL

if [[ -z "$DOWNLOAD_URL" ]]; then
    echo -e "${RED}[Errore] Nessun URL inserito. Uscita.${RESET}"
    exit 1
fi

echo ""
echo -e "${DIM}  Recupero informazioni sul file...${RESET}"

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

echo -e "  [OK] File rilevato: ${BOLD}${REMOTE_FILENAME}${RESET}"

if [[ -n "$FILE_SIZE" && "$FILE_SIZE" -gt 0 ]]; then
    if   (( FILE_SIZE >= 1073741824 )); then
        SIZE_HR=$(awk "BEGIN {printf \"%.2f GB\", $FILE_SIZE/1073741824}")
    elif (( FILE_SIZE >= 1048576 )); then
        SIZE_HR=$(awk "BEGIN {printf \"%.2f MB\", $FILE_SIZE/1048576}")
    else
        SIZE_HR="${FILE_SIZE} B"
    fi
    echo -e "  [OK] Dimensione: ${BOLD}${SIZE_HR}${RESET}"
else
    FILE_SIZE=""
    echo -e "  ${YELLOW}[Avviso] Dimensione non disponibile${RESET}"
fi

echo ""

if [[ -n "$EXT" ]]; then
    echo -e "${BOLD}Nome file di destinazione su PS${RESET} ${DIM}(senza estensione, .${EXT} verrà aggiunta):${RESET}"
else
    echo -e "${BOLD}Nome file di destinazione su PS${RESET} ${DIM}(includi l'estensione se necessaria):${RESET}"
fi

read -rp "  -> " BASE_NAME

if [[ -z "$BASE_NAME" ]]; then
    echo -e "${RED}[Errore] Nome file non inserito. Uscita.${RESET}"
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
echo -e "  ${BOLD}Riepilogo Trasferimento${RESET}"
echo -e "  ${DIM}Sorgente     :${RESET} ${DOWNLOAD_URL}"
echo -e "  ${DIM}PS (FTP)     :${RESET} ${FTP_URL}"
echo -e "${CYAN}---------------------------------------------${RESET}"
echo ""
read -rp "  Confermare e avviare il trasferimento su PS? [S/n] " CONFIRM
CONFIRM="${CONFIRM,,}"
if [[ "$CONFIRM" == "n" || "$CONFIRM" == "no" ]]; then
    echo -e "${YELLOW}  Operazione annullata.${RESET}"
    exit 0
fi

echo ""
echo -e "${BLUE}  Streaming verso la PlayStation in corso...${RESET}"
echo ""

if [[ -n "$FILE_SIZE" ]]; then
    wget -q -O - "$DOWNLOAD_URL" 2>/dev/null \
        | pv -s "$FILE_SIZE" -petrab \
        | curl --silent --show-error --ftp-create-dirs -T - "${FTP_URL}"
else
    wget -q -O - "$DOWNLOAD_URL" 2>/dev/null \
        | pv -petrab \
        | curl --silent --show-error --ftp-create-dirs -T - "${FTP_URL}"
fi

EXIT_CODE=$?

echo ""
if [[ $EXIT_CODE -eq 0 ]]; then
    echo -e "${GREEN}${BOLD}  [OK] File inviato con successo alla PlayStation!${RESET}"
    echo -e "  ${DIM}Disponibile su PS in: /${FTP_DIR}/${DEST_FILENAME}${RESET}"
else
    echo -e "${RED}${BOLD}  [Errore] Trasferimento non riuscito (codice: ${EXIT_CODE})${RESET}"
    echo -e "  ${YELLOW}  - Verifica che la PS sia accesa con GoldHEN / FTP attivo${RESET}"
    echo -e "  ${YELLOW}  - Controlla l'indirizzo IP configurato: ${FTP_HOST}:${FTP_PORT}${RESET}"
    exit $EXIT_CODE
fi

echo ""
