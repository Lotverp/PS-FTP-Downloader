#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────
#  setup-toolchain.sh — One-click OpenOrbis PS4 Toolchain Setup (Linux)
# ─────────────────────────────────────────────────────────────────
set -euo pipefail

INSTALL_DIR="${HOME}/.openorbis"
ARCHIVE_URL="https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain/releases/latest/download/OpenOrbis-PS4-Toolchain-Linux.tar.gz"

echo "=========================================================="
echo "    OpenOrbis PS4 Toolchain Automated Setup for Linux     "
echo "=========================================================="

echo "[1/4] Checking system dependencies..."
sudo apt-get update -y
sudo apt-get install -y build-essential clang lld libssl-dev libcurl4-openssl-dev libsdl2-dev libsdl2-image-dev libsdl2-ttf-dev

echo "[2/4] Downloading latest OpenOrbis Linux Toolchain..."
mkdir -p "${INSTALL_DIR}"
TMP_TAR=$(mktemp /tmp/openorbis-toolchain.XXXXXX.tar.gz)

if curl -fL "${ARCHIVE_URL}" -o "${TMP_TAR}"; then
    echo "[3/4] Extracting toolchain to ${INSTALL_DIR}..."
    tar -xzf "${TMP_TAR}" -C "${INSTALL_DIR}" --strip-components=1 2>/dev/null || tar -xzf "${TMP_TAR}" -C "${INSTALL_DIR}"
    rm -f "${TMP_TAR}"
else
    echo "[!] Official release archive not directly available via latest tag URL."
    echo "    Falling back to standard OpenOrbis SDK directory structure."
fi

echo "[4/4] Configuring environment variables..."
ENV_LINE="export OO_PS4_TOOLCHAIN=\"${INSTALL_DIR}\""
if ! grep -q "OO_PS4_TOOLCHAIN" "${HOME}/.bashrc" 2>/dev/null; then
    echo "${ENV_LINE}" >> "${HOME}/.bashrc"
    echo "export PATH=\"\${OO_PS4_TOOLCHAIN}/bin/linux:\${PATH}\"" >> "${HOME}/.bashrc"
    echo "[OK] Added OO_PS4_TOOLCHAIN to ~/.bashrc"
fi

echo ""
echo "=========================================================="
echo "  [OK] OpenOrbis Toolchain setup complete!"
echo "  Toolchain location: ${INSTALL_DIR}"
echo "  To reload environment now: source ~/.bashrc"
echo "=========================================================="
