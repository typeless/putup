#!/bin/bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2024 pup authors
#
# E2E test runner for pup build system

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PUP_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
PUP_BIN="$PUP_ROOT/pup"
FIXTURES_DIR="$SCRIPT_DIR/fixtures"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

PASSED=0
FAILED=0
SKIPPED=0

log_pass() {
    echo -e "${GREEN}PASS${NC}: $1"
    ((PASSED++))
}

log_fail() {
    echo -e "${RED}FAIL${NC}: $1"
    ((FAILED++))
}

log_skip() {
    echo -e "${YELLOW}SKIP${NC}: $1"
    ((SKIPPED++))
}

log_info() {
    echo -e "     $1"
}

# Check pup binary exists
if [[ ! -x "$PUP_BIN" ]]; then
    echo "Error: pup binary not found at $PUP_BIN"
    echo "Build pup first with: tup"
    exit 1
fi

# Run a single test fixture
run_fixture() {
    local name="$1"
    local fixture_dir="$FIXTURES_DIR/$name"
    local work_dir=$(mktemp -d)

    # Copy fixture to temp directory
    cp -r "$fixture_dir"/* "$work_dir/"

    # Rename Tupfile.fixture to Tupfile (avoids tup scanning fixtures)
    if [[ -f "$work_dir/Tupfile.fixture" ]]; then
        mv "$work_dir/Tupfile.fixture" "$work_dir/Tupfile"
    fi

    # Change to work directory
    pushd "$work_dir" > /dev/null

    # Run the test script if it exists
    if [[ -x "./test.sh" ]]; then
        if PUP="$PUP_BIN" ./test.sh; then
            log_pass "$name"
            popd > /dev/null
            rm -rf "$work_dir"
            return 0
        else
            log_fail "$name"
            log_info "Work dir preserved at: $work_dir"
            popd > /dev/null
            return 1
        fi
    else
        log_skip "$name (no test.sh)"
        popd > /dev/null
        rm -rf "$work_dir"
        return 0
    fi
}

# Main
echo "========================================"
echo "pup E2E Test Suite"
echo "========================================"
echo "pup binary: $PUP_BIN"
echo "fixtures:   $FIXTURES_DIR"
echo ""

# Find and run all fixtures
for fixture in "$FIXTURES_DIR"/*/; do
    if [[ -d "$fixture" ]]; then
        name=$(basename "$fixture")
        run_fixture "$name" || true
    fi
done

echo ""
echo "========================================"
echo "Results: ${GREEN}$PASSED passed${NC}, ${RED}$FAILED failed${NC}, ${YELLOW}$SKIPPED skipped${NC}"
echo "========================================"

if [[ $FAILED -gt 0 ]]; then
    exit 1
fi

exit 0
