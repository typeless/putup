#!/usr/bin/env bash
# Reproducer for pup's transitive implicit-dep tracking bug.
#
# Scenario:
#   1. Source includes only old.h. pup builds, records old.h as implicit dep.
#   2. old.h is modified to transitively include a new header (newhdr.h).
#      pup detects old.h changed and recompiles, but FAILS to add newhdr.h
#      to the source's implicit-dep set.
#   3. newhdr.h is modified. pup reports "Nothing to do." because its
#      stale dep set says the source only depends on old.h (unchanged).
#
# Expected: step 3 rebuilds, .o reflects the new constant.
# Observed: step 3 is a no-op; .o is stale.
#
# Test exits 0 on success (bug fixed), non-zero on failure (bug present).

set -u

PUP="${PUP:-$(command -v pup || echo /home/mural/bin/pup)}"
BUILD_DIR="build/x86"
OBJ="${BUILD_DIR}/src/main.o"

die() {
    echo "FAIL: $*" >&2
    exit 1
}

# Extract the immediate-mode operand of the first mov $0xXX instruction.
# Returns the hex literal as it appears in objdump output (e.g. "0x1").
constant_in_obj() {
    objdump -d "$1" 2>/dev/null \
        | grep -oE 'mov +\$0x[0-9a-f]+' \
        | head -1 \
        | grep -oE '0x[0-9a-f]+'
}

# Decode a hex literal (with 0x prefix) to a decimal integer for printing.
hex_to_dec() {
    printf '%d' "$1" 2>/dev/null
}

# ---------- Step 1: initial build ----------
echo "=== Step 1: initial build (source includes old.h only) ==="
"$PUP" configure -B "$BUILD_DIR" >/dev/null || die "configure failed"
"$PUP" -B "$BUILD_DIR" >/dev/null || die "initial build failed"
[[ -f "$OBJ" ]] || die "expected $OBJ after initial build"

K1="$(constant_in_obj "$OBJ")"
echo "  observed constant in $OBJ: $K1 ($(hex_to_dec "$K1"))"
[[ "$K1" == "0x1" ]] || die "expected 0x1 (ANSWER=1) after step 1, got $K1"

# ---------- Step 2: make old.h transitively include newhdr.h ----------
echo
echo "=== Step 2: modify old.h to #include newhdr.h ==="
cat > include/old.h << 'EOF'
#pragma once
#include "newhdr.h"
#define ANSWER EXTRA
EOF

"$PUP" -B "$BUILD_DIR" >/dev/null || die "rebuild after old.h edit failed"

K2="$(constant_in_obj "$OBJ")"
echo "  observed constant in $OBJ: $K2 ($(hex_to_dec "$K2"))"
[[ "$K2" == "0x64" ]] \
    || die "expected 0x64 (EXTRA=100) after step 2, got $K2 — old.h change not picked up"

# ---------- Forensic between step 2 and 3: did pup record newhdr.h? ----------
# After step 2 the .d file from gcc lists both old.h AND newhdr.h. If pup's
# dep-recording is healthy, both should appear as implicit edges for main.o's
# compile. If newhdr.h is missing here, the bug is in dep recording (post-
# execution .d-file processing), not in change detection.
echo
echo "=== Forensic: pup show index for main.c's compile after step 2 ==="
INDEX_OUT="$("$PUP" show index -B "$BUILD_DIR" main.c 2>&1)"
echo "$INDEX_OUT" | sed 's/^/  /'

if echo "$INDEX_OUT" | grep -q "implicit:.*newhdr.h"; then
    echo "  forensic: newhdr.h IS recorded as an implicit dep — proceed"
else
    echo
    echo "  forensic: newhdr.h is NOT in main.c's implicit-dep set."
    echo "            Root cause: pup's post-execution .d-file processing did"
    echo "            not extend the implicit-dep set with newly-discovered"
    echo "            transitive headers."
fi

# ---------- Step 3: modify newhdr.h (the transitive header) ----------
echo
echo "=== Step 3: modify newhdr.h (EXTRA 100 -> 7777) ==="
cat > include/newhdr.h << 'EOF'
#pragma once
#define EXTRA 7777
EOF

OUT="$("$PUP" -B "$BUILD_DIR" 2>&1)"
EC=$?
echo "  pup output: $OUT"
[[ $EC -eq 0 ]] || die "rebuild returned non-zero: $EC"

K3="$(constant_in_obj "$OBJ")"
echo "  observed constant in $OBJ: $K3 ($(hex_to_dec "$K3"))"

if [[ "$OUT" == *"Nothing to do"* ]]; then
    echo
    echo "BUG PRESENT: pup reported 'Nothing to do' after newhdr.h change."
    echo "             newhdr.h is transitively included by old.h, which is in"
    echo "             the source's implicit-dep set, but newhdr.h itself was"
    echo "             never recorded — confirmed by 'pup show index' above."
fi

if [[ "$K3" != "0x1e61" ]]; then
    echo
    echo "Diagnostic:"
    echo "  expected $OBJ to contain 0x1e61 (EXTRA=7777)"
    echo "  actually  contains $K3 ($(hex_to_dec "$K3"))"
    echo "  this means main.c was NOT recompiled after newhdr.h changed"
    die "transitive header change was not detected"
fi

echo
echo "PASS: pup detected transitive header change."
