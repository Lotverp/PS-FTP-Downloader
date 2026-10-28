#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────────────
#  run_tests.sh — P2C Downloader Comprehensive Test Suite
#
#  Verifies all core components:
#    1. Catalog JSON Schema & HTTP Reachability (icons, packages)
#    2. Native C Core Unit Tests (link parsing, regex, slug, ext, catalog)
#    3. Resolver & Browser Download Safety (CDP download deny, sandbox)
#    4. Archive Tools & Multi-Part Extraction Pipeline (7z, unar, etc.)
#    5. GTK Application Compilation & Headless UI Smoke Test (fatal criticals)
# ─────────────────────────────────────────────────────────────────────────────

set -o pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "${SCRIPT_DIR}"

BOLD="\033[1m"
GREEN="\033[32m"
RED="\033[31m"
YELLOW="\033[33m"
CYAN="\033[36m"
RESET="\033[0m"

echo -e "${BOLD}${CYAN}========================================================================${RESET}"
echo -e "${BOLD}${CYAN}             P2C DOWNLOADER — COMPREHENSIVE TEST SUITE                  ${RESET}"
echo -e "${BOLD}${CYAN}========================================================================${RESET}"
echo ""

START_TIME=$(date +%s)
FAILED_TESTS=()
PASSED_TESTS=()

run_suite() {
    local suite_name="$1"
    local command="$2"

    echo -e "${BOLD}▶ Running: ${suite_name}...${RESET}"
    if eval "${command}"; then
        echo -e "${BOLD}${GREEN}✔ [PASS] ${suite_name}${RESET}\n"
        PASSED_TESTS+=("${suite_name}")
    else
        echo -e "${BOLD}${RED}✘ [FAIL] ${suite_name}${RESET}\n"
        FAILED_TESTS+=("${suite_name}")
    fi
}

# 1. Catalog Integrity & Schema
run_suite "Catalog Schema & Reachability" "python3 tests/test_catalog.py"

# 2. C Core Unit Tests
run_suite "C Core & Link Parser Unit Tests" "
    gcc \$(pkg-config --cflags gtk4 libcurl json-glib-1.0 gdk-pixbuf-2.0) \
        -DP2C_UNIT_TESTING=1 -O2 -Wall -Wextra \
        -o tests/test_c_core tests/test_c_core.c \
        \$(pkg-config --libs gtk4 libcurl json-glib-1.0 gdk-pixbuf-2.0) -lpthread -lm && \
    ./tests/test_c_core
"

# 3. Resolver & Browser Download Prevention
run_suite "Resolver & Browser Download Safety" "python3 tests/test_resolver.py"

# 4. Archive Tools Pipeline
run_suite "Archive Tools & Extraction Pipeline" "python3 tests/test_archive_tools.py"

# 5. GTK UI Smoke Test
run_suite "GTK Application Compilation & Smoke Test" "bash tests/test_smoke.sh"

END_TIME=$(date +%s)
DURATION=$((END_TIME - START_TIME))

echo -e "${BOLD}${CYAN}========================================================================${RESET}"
echo -e "${BOLD}${CYAN}                          TEST SUITE SUMMARY                            ${RESET}"
echo -e "${BOLD}${CYAN}========================================================================${RESET}"
echo -e "Total Execution Time: ${DURATION}s"
echo -e "Passed: ${GREEN}${BOLD}${#PASSED_TESTS[@]}${RESET}"
echo -e "Failed: ${RED}${BOLD}${#FAILED_TESTS[@]}${RESET}"

if [ ${#FAILED_TESTS[@]} -eq 0 ]; then
    echo -e "\n${BOLD}${GREEN}🎉 ALL TEST SUITES PASSED SUCCESSFULLY! Everything is operational.${RESET}\n"
    exit 0
else
    echo -e "\n${BOLD}${RED}⚠️  THE FOLLOWING TEST SUITES FAILED:${RESET}"
    for f in "${FAILED_TESTS[@]}"; do
        echo -e "  - ${RED}${f}${RESET}"
    done
    echo ""
    exit 1
fi
