#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────
#  run.sh — Avvia P2C Downloader direttamente dai sorgenti
#           senza bisogno di pacchetti .deb o permessi di root
#
#  Uso:
#    ./run.sh
# ─────────────────────────────────────────────────────────────────

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
GTK_DIR="${SCRIPT_DIR}/gtk-app"

# Se il binario non esiste o i sorgenti sono stati modificati, compila al volo
if [ ! -f "${GTK_DIR}/p2c-downloader" ] || [ "${GTK_DIR}/main.c" -nt "${GTK_DIR}/p2c-downloader" ]; then
    echo "[P2C] Compilazione in corso..."
    make -C "${GTK_DIR}"
fi

echo "[P2C] Avvio P2C Downloader..."
cd "${GTK_DIR}"
exec ./p2c-downloader "$@"
