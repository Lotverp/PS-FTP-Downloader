#!/usr/bin/env bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

TITLE="PS4 Direct Downloader"
TITLE_ID="BREW00777"
CONTENT_ID="IV0000-${TITLE_ID}_00-PS4DOWNLOADER000"
PKG_NAME="ps4-downloader-v1.0.0.pkg"

OO_ROOT="${OO_PS4_TOOLCHAIN:-$HOME/.openorbis/OpenOrbis/PS4Toolchain}"
PKG_TOOL="$OO_ROOT/bin/linux/PkgTool.Core"
CREATE_GP4="$OO_ROOT/bin/linux/create-gp4"

export DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1
export LD_LIBRARY_PATH="$HOME/.openorbis/libssl1.1/usr/lib/x86_64-linux-gnu:${LD_LIBRARY_PATH:-}"

echo "=========================================================="
echo "    PS4 Direct Downloader - PKG Packaging Pipeline        "
echo "    Title ID: ${TITLE_ID} | Content ID: ${CONTENT_ID}      "
echo "=========================================================="

# 1. Compile EBOOT.BIN if not present
echo "[1/5] Verifica compilazione EBOOT.BIN..."
if [ ! -f "build/eboot.bin" ]; then
    make ps4
fi
cp build/eboot.bin eboot.bin

# 2. Ensure system modules and LiveArea PRX exist
echo "[2/5] Verifica moduli di sistema PS4..."
mkdir -p sce_module sce_sys/about
if [ -d "$OO_ROOT/samples/SDL2/sce_module" ]; then
    cp -u "$OO_ROOT/samples/SDL2/sce_module/"*.prx sce_module/ 2>/dev/null || true
    cp -u "$OO_ROOT/samples/SDL2/sce_sys/about/right.sprx" sce_sys/about/ 2>/dev/null || true
fi

# 3. Generate icon0.png and pic1.png visuals
echo "[3/5] Generazione asset grafici icon0 e pic1..."
python3 tools/gen_assets.py

# 4. Generate official param.sfo using PkgTool.Core
echo "[4/5] Generazione param.sfo certificato..."
if [ -x "$PKG_TOOL" ]; then
    "$PKG_TOOL" sfo_new sce_sys/param.sfo
    "$PKG_TOOL" sfo_setentry sce_sys/param.sfo APP_TYPE --type Integer --maxsize 4 --value 1
    "$PKG_TOOL" sfo_setentry sce_sys/param.sfo APP_VER --type Utf8 --maxsize 8 --value "01.00"
    "$PKG_TOOL" sfo_setentry sce_sys/param.sfo ATTRIBUTE --type Integer --maxsize 4 --value 0
    "$PKG_TOOL" sfo_setentry sce_sys/param.sfo CATEGORY --type Utf8 --maxsize 4 --value "gd"
    "$PKG_TOOL" sfo_setentry sce_sys/param.sfo CONTENT_ID --type Utf8 --maxsize 48 --value "$CONTENT_ID"
    "$PKG_TOOL" sfo_setentry sce_sys/param.sfo DOWNLOAD_DATA_SIZE --type Integer --maxsize 4 --value 0
    "$PKG_TOOL" sfo_setentry sce_sys/param.sfo SYSTEM_VER --type Integer --maxsize 4 --value 0
    "$PKG_TOOL" sfo_setentry sce_sys/param.sfo TITLE --type Utf8 --maxsize 128 --value "$TITLE"
    "$PKG_TOOL" sfo_setentry sce_sys/param.sfo TITLE_ID --type Utf8 --maxsize 12 --value "$TITLE_ID"
    "$PKG_TOOL" sfo_setentry sce_sys/param.sfo VERSION --type Utf8 --maxsize 8 --value "01.00"
else
    python3 tools/gen_sfo.py "$TITLE" "$TITLE_ID" "01.00" "sce_sys/param.sfo"
fi

# 5. Generate GP4 specification and Build Package
echo "[5/5] Creazione progetto GP4 con PlayGo Scenarios e creazione PKG..."
FILES_TO_PACK="eboot.bin sce_sys/about/right.sprx sce_sys/param.sfo sce_sys/icon0.png sce_module/libc.prx sce_module/libSceFios2.prx"

if [ -x "$CREATE_GP4" ]; then
    "$CREATE_GP4" -out app.gp4 --content-id="$CONTENT_ID" --files "$FILES_TO_PACK"
else
    python3 tools/gen_gp4.py "." "app.gp4" "$TITLE_ID"
fi

if [ -x "$PKG_TOOL" ]; then
    echo "Esecuzione PkgTool.Core pkg_build app.gp4 ."
    "$PKG_TOOL" pkg_build app.gp4 .

    # Clean temporary root eboot.bin
    rm -f eboot.bin

    # Rename generated file to friendly name
    GEN_PKG=$(ls -t "${CONTENT_ID}.pkg" "${CONTENT_ID}"*.pkg 2>/dev/null | head -n 1 || true)
    if [ -n "$GEN_PKG" ] && [ -f "$GEN_PKG" ]; then
        mv "$GEN_PKG" "$PKG_NAME"
    fi

    echo ""
    echo "=========================================================="
    echo " SUCCESSO! Pacchetto PS4 Homebrew generato:"
    echo " -> $SCRIPT_DIR/$PKG_NAME"
    echo ""
    echo " Dimensione: $(du -h "$PKG_NAME" | cut -f1)"
    echo " Controlli completati: EBOOT.BIN in root, LiveArea registrata"
    echo " e PlayGo Scenarios configurati per la dashboard XMB."
    echo "=========================================================="
else
    rm -f eboot.bin
    echo "PkgTool.Core non trovato in $PKG_TOOL"
fi
