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
    echo "Compilazione EBOOT.BIN per PS4 con OpenOrbis..."
    make ps4
fi

# 3. Generate app.gp4 project file
echo "[3/4] Generazione progetto GP4 per il packaging fPKG..."
python3 tools/gen_gp4.py

# 4. Create PKG
echo "[4/4] Creazione pacchetto PS4 .pkg..."
PKG_NAME="ps4-downloader-v1.0.0.pkg"

OO_ROOT="${OO_PS4_TOOLCHAIN:-$HOME/.openorbis/OpenOrbis/PS4Toolchain}"
PKG_TOOL="$OO_ROOT/bin/linux/PkgTool.Core"

if [ -x "$PKG_TOOL" ]; then
    echo "Esecuzione PkgTool.Core pkg_build app.gp4 ."
    export DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1
    export LD_LIBRARY_PATH="$HOME/.openorbis/libssl1.1/usr/lib/x86_64-linux-gnu:${LD_LIBRARY_PATH:-}"
    "$PKG_TOOL" pkg_build app.gp4 .

    # Rename generated IV0001-PSFT00001_00-PS4DOWNLOADER000.pkg to friendly name
    GEN_PKG=$(ls -t IV0001-PSFT00001*.pkg 2>/dev/null | head -n 1 || true)
    if [ -n "$GEN_PKG" ] && [ -f "$GEN_PKG" ]; then
        mv "$GEN_PKG" "$PKG_NAME"
    fi

    echo ""
    echo "=========================================================="
    echo " SUCCESSO! Pacchetto PS4 generato:"
    echo " -> $SCRIPT_DIR/$PKG_NAME"
    echo ""
    echo " Dimensione: $(du -h "$PKG_NAME" | cut -f1)"
    echo " Copia questo file su chiavetta USB (exFAT)"
    echo " e installalo sulla tua PS4 da 'Package Installer' (GoldHEN)!"
    echo "=========================================================="
elif command -v orbis-pub-cmd &> /dev/null; then
    orbis-pub-cmd img_create app.gp4 "$PKG_NAME"
    echo "SUCCESSO! Pacchetto generato: $SCRIPT_DIR/$PKG_NAME"
else
    echo "----------------------------------------------------------"
    echo "Il progetto GP4 e tutti gli assets sono pronti in:"
    echo "  - $SCRIPT_DIR/app.gp4"
    echo "  - $SCRIPT_DIR/build/eboot.bin"
    echo "----------------------------------------------------------"
fi
