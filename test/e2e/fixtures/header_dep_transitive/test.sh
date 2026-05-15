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
# Expected: step 3 rebuilds, the linked program reflects the new constant.
# Observed: step 3 is a no-op; program is stale.
#
# Test exits 0 on success (bug fixed), non-zero on failure (bug present).
#
# Oracle: link main.c into a tiny program whose main() returns the constant.
# Running it and reading $? is portable across Linux GNU-binutils and macOS
# LLVM toolchains. Constants stay < 256 so the exit code carries them
# unmodified (POSIX masks exit codes to 8 bits).

set -u

PUP="${PUP:-$(command -v pup || echo /home/mural/bin/pup)}"
BUILD_DIR="build/x86"
OBJ="${BUILD_DIR}/src/main.o"
PROG="${BUILD_DIR}/src/program"

die() {
    echo "FAIL: $*" >&2
    exit 1
}

# Run the program and report its exit code (= the constant baked into main.o).
constant_in_prog() {
    "$1"
    echo $?
}

# ---------- Step 1: initial build ----------
echo "=== Step 1: initial build (source includes old.h only) ==="
"$PUP" configure -B "$BUILD_DIR" >/dev/null || die "configure failed"
"$PUP" -B "$BUILD_DIR" >/dev/null || die "initial build failed"
[[ -f "$OBJ" ]] || die "expected $OBJ after initial build"
[[ -x "$PROG" ]] || die "expected $PROG after initial build"

K1="$("$PROG"; echo $?)"
echo "  $PROG exits with: $K1"
[[ "$K1" == "1" ]] || die "expected exit 1 (ANSWER=1) after step 1, got $K1"

# ---------- Step 2: make old.h transitively include newhdr.h ----------
echo
echo "=== Step 2: modify old.h to #include newhdr.h ==="
cat > include/old.h << 'EOF'
#pragma once
#include "newhdr.h"
#define ANSWER EXTRA
EOF

"$PUP" -B "$BUILD_DIR" >/dev/null || die "rebuild after old.h edit failed"

K2="$("$PROG"; echo $?)"
echo "  $PROG exits with: $K2"
[[ "$K2" == "100" ]] \
    || die "expected exit 100 (EXTRA=100) after step 2, got $K2 — old.h change not picked up"

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
echo "=== Step 3: modify newhdr.h (EXTRA 100 -> 77) ==="
cat > include/newhdr.h << 'EOF'
#pragma once
#define EXTRA 77
EOF

OUT="$("$PUP" -B "$BUILD_DIR" 2>&1)"
EC=$?
echo "  pup output: $OUT"
[[ $EC -eq 0 ]] || die "rebuild returned non-zero: $EC"

K3="$("$PROG"; echo $?)"
echo "  $PROG exits with: $K3"

if [[ "$OUT" == *"Nothing to do"* ]]; then
    echo
    echo "BUG PRESENT: pup reported 'Nothing to do' after newhdr.h change."
    echo "             newhdr.h is transitively included by old.h, which is in"
    echo "             the source's implicit-dep set, but newhdr.h itself was"
    echo "             never recorded — confirmed by 'pup show index' above."
fi

if [[ "$K3" != "77" ]]; then
    echo
    echo "Diagnostic:"
    echo "  expected $PROG to exit 77 (EXTRA=77)"
    echo "  actually exits $K3"
    echo "  this means main.c was NOT recompiled after newhdr.h changed"
    die "transitive header change was not detected"
fi

echo
echo "PASS: pup detected transitive header change."
