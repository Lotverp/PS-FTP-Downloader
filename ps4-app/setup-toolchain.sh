#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────
#  setup-toolchain.sh — One-click OpenOrbis PS4 Toolchain Setup (Linux)
# ─────────────────────────────────────────────────────────────────
set -euo pipefail

INSTALL_DIR="${HOME}/.openorbis"
TOOLCHAIN_DIR="${INSTALL_DIR}/OpenOrbis/PS4Toolchain"
ARCHIVE_URL="https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain/releases/download/v0.5.4/toolchain-llvm-18.tar.gz"
LIBSSL_URL="http://security.ubuntu.com/ubuntu/pool/main/o/openssl/libssl1.1_1.1.1f-1ubuntu2.24_amd64.deb"

echo "=========================================================="
echo "    OpenOrbis PS4 Toolchain Automated Setup for Linux     "
echo "=========================================================="

mkdir -p "${INSTALL_DIR}"

# 1. Download OpenOrbis SDK & Tools
if [ ! -d "${TOOLCHAIN_DIR}" ]; then
    echo "[1/4] Scaricamento OpenOrbis PS4 Toolchain v0.5.4..."
    TMP_TAR=$(mktemp /tmp/openorbis-toolchain.XXXXXX.tar.gz)
    curl -fL "${ARCHIVE_URL}" -o "${TMP_TAR}"
    echo "Estrazione toolchain in ${INSTALL_DIR}..."
    tar -xzf "${TMP_TAR}" -C "${INSTALL_DIR}"
    rm -f "${TMP_TAR}"
else
    echo "[1/4] OpenOrbis PS4 Toolchain già presente in ${TOOLCHAIN_DIR}."
fi

# 2. Download and configure Clang 18 & LLD (user-space without root)
if [ ! -x "${INSTALL_DIR}/bin/clang" ]; then
    echo "[2/4] Configurazione Clang 18 e LLD 18 per compilazione PS4 (FreeBSD 12)..."
    TMP_LLVM_DIR=$(mktemp -d /tmp/llvm-debs.XXXXXX)
    cd "${TMP_LLVM_DIR}"
    apt-get download clang-18 lld-18 libllvm18 libclang-common-18-dev libclang-cpp18 libclang1-18 llvm-18-linker-tools
    mkdir -p "${INSTALL_DIR}/llvm"
    for f in *.deb; do dpkg -x "$f" "${INSTALL_DIR}/llvm"; done
    cd - >/dev/null
    rm -rf "${TMP_LLVM_DIR}"

    mkdir -p "${INSTALL_DIR}/bin"
    cat << 'EOF' > "${INSTALL_DIR}/bin/clang"
#!/usr/bin/env bash
export LD_LIBRARY_PATH="${HOME}/.openorbis/llvm/usr/lib/x86_64-linux-gnu:${LD_LIBRARY_PATH:-}"
exec "${HOME}/.openorbis/llvm/usr/bin/clang-18" "$@"
EOF

    cat << 'EOF' > "${INSTALL_DIR}/bin/clang++"
#!/usr/bin/env bash
export LD_LIBRARY_PATH="${HOME}/.openorbis/llvm/usr/lib/x86_64-linux-gnu:${LD_LIBRARY_PATH:-}"
exec "${HOME}/.openorbis/llvm/usr/bin/clang++-18" "$@"
EOF

    cat << 'EOF' > "${INSTALL_DIR}/bin/ld.lld"
#!/usr/bin/env bash
export LD_LIBRARY_PATH="${HOME}/.openorbis/llvm/usr/lib/x86_64-linux-gnu:${LD_LIBRARY_PATH:-}"
exec "${HOME}/.openorbis/llvm/usr/bin/ld.lld-18" "$@"
EOF

    chmod +x "${INSTALL_DIR}/bin/clang" "${INSTALL_DIR}/bin/clang++" "${INSTALL_DIR}/bin/ld.lld"
    echo "[OK] Clang e LLD configurati in ${INSTALL_DIR}/bin"
else
    echo "[2/4] Clang e LLD già presenti in ${INSTALL_DIR}/bin."
fi

# 3. Setup OpenSSL 1.1 runtime compatibility for PkgTool.Core
if [ ! -d "${INSTALL_DIR}/libssl1.1" ]; then
    echo "[3/4] Configurazione compatibilità OpenSSL 1.1 per PkgTool.Core..."
    TMP_SSL=$(mktemp /tmp/libssl.XXXXXX.deb)
    curl -fL "${LIBSSL_URL}" -o "${TMP_SSL}"
    mkdir -p "${INSTALL_DIR}/libssl1.1"
    dpkg -x "${TMP_SSL}" "${INSTALL_DIR}/libssl1.1"
    rm -f "${TMP_SSL}"
    echo "[OK] OpenSSL 1.1 configurato in ${INSTALL_DIR}/libssl1.1"
else
    echo "[3/4] OpenSSL 1.1 già presente in ${INSTALL_DIR}/libssl1.1."
fi

# 4. Environment Variables
echo "[4/4] Configurazione variabili d'ambiente..."
ENV_LINE="export OO_PS4_TOOLCHAIN=\"${TOOLCHAIN_DIR}\""
if ! grep -q "OO_PS4_TOOLCHAIN" "${HOME}/.bashrc" 2>/dev/null; then
    echo "" >> "${HOME}/.bashrc"
    echo "# OpenOrbis PS4 Toolchain" >> "${HOME}/.bashrc"
    echo "${ENV_LINE}" >> "${HOME}/.bashrc"
    echo "export PATH=\"\${OO_PS4_TOOLCHAIN}/bin/linux:\${HOME}/.openorbis/bin:\${PATH}\"" >> "${HOME}/.bashrc"
    echo "[OK] Variabili aggiunte a ~/.bashrc"
fi

echo ""
echo "=========================================================="
echo "  [OK] Setup OpenOrbis completato con successo!"
echo "  Toolchain location: ${TOOLCHAIN_DIR}"
echo "  Puoi compilare direttamente con:"
echo "    make ps4"
echo "    ./build-pkg.sh"
echo "=========================================================="
