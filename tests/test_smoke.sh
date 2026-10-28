#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────
#  tests/test_smoke.sh — Headless GTK UI Smoke Test
#  Ensures compilation succeeds and the GTK4 application builds its
#  entire UI, dropdown models, and lifecycle with zero GLib criticals.
# ─────────────────────────────────────────────────────────────────

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
GTK_DIR="${ROOT_DIR}/gtk-app"

echo "--- [TEST 5/5] Running GTK Application Compilation & Smoke Test ---"

echo "  -> Compiling GTK application (make -C gtk-app)..."
make -C "${GTK_DIR}" > /dev/null
echo "    [OK] Compilation succeeded"

if [ ! -x "${GTK_DIR}/p2c-downloader" ]; then
    echo "  [FAIL] Binary ${GTK_DIR}/p2c-downloader not found or not executable"
    exit 1
fi

echo "  -> Running smoke test with G_DEBUG=fatal-criticals..."
OUT=$(G_DEBUG=fatal-criticals "${GTK_DIR}/p2c-downloader" --smoke-test 2>&1)
EXIT_CODE=$?

if [ $EXIT_CODE -ne 0 ]; then
    echo "  [FAIL] Smoke test exited with code ${EXIT_CODE}"
    echo "${OUT}"
    exit 1
fi

if ! echo "${OUT}" | grep -q "\[SMOKE TEST\] Application and UI successfully initialized"; then
    echo "  [FAIL] Smoke test did not report successful initialization"
    echo "${OUT}"
    exit 1
fi

echo "    [OK] GTK UI initialized and exited cleanly with zero fatal warnings"
echo "  [PASS] GTK application smoke test completed successfully!"
echo ""
exit 0
