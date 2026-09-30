#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────
#  build-deb.sh — Create an installable .deb package for
#                 PS FTP Downloader (GTK4)
#
#  Usage: ./build-deb.sh
#  Output: ../ps-ftp-downloader_1.1.0_amd64.deb
# ─────────────────────────────────────────────────────────────────

set -euo pipefail

PKG_NAME="ps-ftp-downloader"
PKG_VERSION="1.1.0"
PKG_ARCH="amd64"
PKG_MAINTAINER="Your Name <your@email.com>"
PKG_DESCRIPTION="Download PKG directly from the network to PS via FTP without local storage"
PKG_DEPENDS="libgtk-4-1, libcurl4"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/../.deb-build"
PKG_ROOT="${BUILD_DIR}/${PKG_NAME}_${PKG_VERSION}_${PKG_ARCH}"
OUTPUT="${SCRIPT_DIR}/../${PKG_NAME}_${PKG_VERSION}_${PKG_ARCH}.deb"

BOLD="\033[1m"; GREEN="\033[1;32m"; CYAN="\033[1;36m"; RED="\033[1;31m"; RESET="\033[0m"

echo -e "${CYAN}"
echo "  ============================================"
echo "      PS FTP Downloader — .deb Builder        "
echo "  ============================================"
echo -e "${RESET}"

echo -e "${BOLD}[1/4] Checking build dependencies...${RESET}"
MISSING=()
for cmd in gcc pkg-config dpkg-deb; do command -v "$cmd" &>/dev/null || MISSING+=("$cmd"); done
if ! pkg-config --exists gtk4 2>/dev/null; then MISSING+=("libgtk-4-dev"); fi
if ! pkg-config --exists libcurl 2>/dev/null; then MISSING+=("libcurl4-openssl-dev"); fi
if [ ${#MISSING[@]} -gt 0 ]; then
    echo -e "${RED}Missing: ${MISSING[*]}${RESET}"
    echo "  sudo apt-get install -y build-essential libgtk-4-dev libcurl4-openssl-dev"
    exit 1
fi
echo -e "  [OK] Dependencies satisfied"

echo -e "${BOLD}[2/4] Compiling...${RESET}"
cd "${SCRIPT_DIR}"
make clean 2>/dev/null || true
make
echo -e "  [OK] Binary built: ${SCRIPT_DIR}/ps-ftp-downloader"

echo -e "${BOLD}[3/4] Creating package structure...${RESET}"
rm -rf "${PKG_ROOT}"
mkdir -p \
    "${PKG_ROOT}/DEBIAN" \
    "${PKG_ROOT}/usr/bin" \
    "${PKG_ROOT}/usr/share/ps-ftp-downloader" \
    "${PKG_ROOT}/usr/share/applications" \
    "${PKG_ROOT}/usr/share/pixmaps" \
    "${PKG_ROOT}/usr/share/doc/${PKG_NAME}"

INSTALLED_SIZE=$(du -sk "${SCRIPT_DIR}/ps-ftp-downloader" | cut -f1)

cat > "${PKG_ROOT}/DEBIAN/control" << EOT
Package: ${PKG_NAME}
Version: ${PKG_VERSION}
Architecture: ${PKG_ARCH}
Maintainer: ${PKG_MAINTAINER}
Installed-Size: ${INSTALLED_SIZE}
Depends: ${PKG_DEPENDS}
Section: net
Priority: optional
Homepage: https://github.com/Lotverp/PS-FTP-Downloader
Description: ${PKG_DESCRIPTION}
 PS FTP Downloader downloads PKG files directly from any URL
 and sends them to the PS via FTP in real time, using a
 2 MB ring-buffer pipe. No temporary files, zero local storage.
 .
 Compatible with: GoldHen, ps4-hen, and any PS FTP server.
 .
 Features:
  - Modern GTK4 interface with dark theme
  - Multiple concurrent downloads
  - Pause and Resume support
  - Progress bar with real-time speed and ETA
  - Automatic file extension detection
  - Pure HTTP to FTP streaming (2 MB ring-buffer)
EOT

cat > "${PKG_ROOT}/DEBIAN/postinst" << 'EOF2'
#!/bin/sh
set -e
if command -v gtk-update-icon-cache >/dev/null 2>&1; then
    gtk-update-icon-cache -f -t /usr/share/icons/hicolor >/dev/null 2>&1 || true
fi
if command -v update-desktop-database >/dev/null 2>&1; then
    update-desktop-database >/dev/null 2>&1 || true
fi
EOF2
chmod 0755 "${PKG_ROOT}/DEBIAN/postinst"

cat > "${PKG_ROOT}/DEBIAN/postrm" << 'EOF3'
#!/bin/sh
set -e
if command -v gtk-update-icon-cache >/dev/null 2>&1; then
    gtk-update-icon-cache -f -t /usr/share/icons/hicolor >/dev/null 2>&1 || true
fi
if command -v update-desktop-database >/dev/null 2>&1; then
    update-desktop-database >/dev/null 2>&1 || true
fi
EOF3
chmod 0755 "${PKG_ROOT}/DEBIAN/postrm"

install -m 0755 "${SCRIPT_DIR}/ps-ftp-downloader"           "${PKG_ROOT}/usr/bin/ps-ftp-downloader"
install -m 0644 "${SCRIPT_DIR}/style.css"                     "${PKG_ROOT}/usr/share/ps-ftp-downloader/style.css"
install -m 0644 "${SCRIPT_DIR}/ps-ftp-downloader.desktop"   "${PKG_ROOT}/usr/share/applications/ps-ftp-downloader.desktop"
install -m 0644 "${SCRIPT_DIR}/github-mark.svg"             "${PKG_ROOT}/usr/share/ps-ftp-downloader/github-mark.svg"

# Check if the PNG icon exists, otherwise use SVG fallback
if [ -f "${SCRIPT_DIR}/ps-ftp-downloader.png" ]; then
    install -m 0644 "${SCRIPT_DIR}/ps-ftp-downloader.png" "${PKG_ROOT}/usr/share/pixmaps/ps-ftp-downloader.png"
else
    mkdir -p "${PKG_ROOT}/usr/share/icons/hicolor/scalable/apps"
    cat > "${PKG_ROOT}/usr/share/icons/hicolor/scalable/apps/ps-ftp-downloader.svg" << 'SVGEOF'
<?xml version="1.0" encoding="UTF-8"?>
<svg width="64" height="64" viewBox="0 0 64 64" xmlns="http://www.w3.org/2000/svg">
  <rect width="64" height="64" rx="14" fill="#13132a"/>
  <text x="32" y="44" text-anchor="middle" font-size="28" fill="#a5b4fc" font-family="sans-serif">PS</text>
</svg>
SVGEOF
fi

cat > "${PKG_ROOT}/usr/share/doc/${PKG_NAME}/copyright" << EOT
Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/
Upstream-Name: ps-ftp-downloader
Source: https://github.com/Lotverp/PS-FTP-Downloader

Files: *
Copyright: $(date +%Y) ${PKG_MAINTAINER}
License: MIT
EOT

echo -e "  [OK] Structure created"

echo -e "${BOLD}[4/4] Building .deb...${RESET}"
dpkg-deb --build --root-owner-group "${PKG_ROOT}" "${OUTPUT}"

echo ""
echo -e "${GREEN}${BOLD}  [OK] Package created successfully!${RESET}"
echo -e "  File: ${OUTPUT}"
echo -e "  Size: $(du -sh "${OUTPUT}" | cut -f1)"
echo ""
echo -e "  Install with:"
echo -e "  ${BOLD}sudo apt install \"${OUTPUT}\"${RESET}"
echo ""
