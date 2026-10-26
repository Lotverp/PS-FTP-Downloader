#!/usr/bin/env bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

echo "=========================================================="
echo "    PS4 Direct Downloader - PKG Packaging Pipeline        "
echo "=========================================================="

# 1. Ensure sce_sys assets and param.sfo exist
echo "[1/4] Generazione metadati e assets SFO/PNG..."
mkdir -p sce_sys
python3 tools/gen_sfo.py
python3 tools/gen_assets.py

# 2. Check eboot.bin
echo "[2/4] Verifica EBOOT.BIN..."
if [ ! -f "build/eboot.bin" ]; then
    if [ -d "${OO_PS4_TOOLCHAIN:-/opt/OpenOrbis/PS4Toolchain}" ]; then
        echo "Compilazione EBOOT.BIN con OpenOrbis..."
        make ps4
    else
        echo "AVVISO: build/eboot.bin non trovato."
        echo "Se non hai ancora compilato l'EBOOT PS4, esegui './setup-toolchain.sh' seguito da 'make ps4'."
        echo "Creazione segnaposto eboot per generare il progetto GP4..."
        mkdir -p build
        touch build/eboot.bin
    fi
fi

# 3. Generate app.gp4 project file
echo "[3/4] Generazione progetto GP4 per il packaging fPKG..."
python3 tools/gen_gp4.py

# 4. Create PKG
echo "[4/4] Creazione pacchetto PS4 .pkg..."
PKG_NAME="ps4-downloader-v1.0.0.pkg"

# Check for native orbis-pub-cmd or wine orbis-pub-cmd
PUB_CMD=""
if command -v orbis-pub-cmd &> /dev/null; then
    PUB_CMD="orbis-pub-cmd"
elif [ -n "$OO_PS4_TOOLCHAIN" ] && [ -x "$OO_PS4_TOOLCHAIN/bin/linux/orbis-pub-cmd" ]; then
    PUB_CMD="$OO_PS4_TOOLCHAIN/bin/linux/orbis-pub-cmd"
elif [ -x "/opt/OpenOrbis/PS4Toolchain/bin/linux/orbis-pub-cmd" ]; then
    PUB_CMD="/opt/OpenOrbis/PS4Toolchain/bin/linux/orbis-pub-cmd"
fi

if [ -n "$PUB_CMD" ]; then
    echo "Esecuzione orbis-pub-cmd: $PUB_CMD img_create app.gp4 $PKG_NAME"
    "$PUB_CMD" img_create app.gp4 "$PKG_NAME"
    echo ""
    echo "=========================================================="
    echo " SUCCESSO! Pacchetto generato: $SCRIPT_DIR/$PKG_NAME"
    echo " Copia questo file su una chiavetta USB (exFAT/FAT32)"
    echo " e installalo sulla tua PS4 da 'Package Installer' (GoldHEN)."
    echo "=========================================================="
else
    echo "----------------------------------------------------------"
    echo "Il progetto GP4 e tutti gli assets sono pronti in:"
    echo "  - $SCRIPT_DIR/app.gp4"
    echo "  - $SCRIPT_DIR/sce_sys/param.sfo"
    echo "  - $SCRIPT_DIR/sce_sys/icon0.png"
    echo "  - $SCRIPT_DIR/sce_sys/pic1.png"
    echo "  - $SCRIPT_DIR/build/eboot.bin"
    echo ""
    echo "Per generare il file .pkg finale, puoi utilizzare:"
    echo "  1) 'orbis-pub-cmd img_create app.gp4 $PKG_NAME' (su Linux con OpenOrbis)"
    echo "  2) 'wine orbis-pub-cmd.exe img_create app.gp4 $PKG_NAME' (con Fake PKG Generator)"
    echo "  3) Fake PKG Generator UI (orbis-pub-gen.exe): apri app.gp4 e clicca 'Build'"
    echo "----------------------------------------------------------"
fi
