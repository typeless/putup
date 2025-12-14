#!/bin/bash
# Test: pup distclean -n (dry-run) doesn't actually remove files
set -e

$PUP init
$PUP build

# Verify setup
if [[ ! -f "hello.o" ]]; then
    echo "FAIL: hello.o not created after build"
    exit 1
fi
if [[ ! -d ".pup" ]]; then
    echo "FAIL: .pup/ not created after build"
    exit 1
fi

# Dry-run distclean
output=$($PUP distclean -n 2>&1)

# Verify "Would remove" in output
if ! echo "$output" | grep -q "Would remove"; then
    echo "FAIL: Dry-run should report 'Would remove', got:"
    echo "$output"
    exit 1
fi

# Verify all files still exist after dry-run (including output and .pup/)
remaining=$(find . -type f ! -path './.pup/*' ! -name 'test.sh' | sort)
expected=$(printf "./hello.c\n./hello.o\n./Tupfile" | sort)
if [[ "$remaining" != "$expected" ]]; then
    echo "FAIL: Dry-run should not remove files. Expected:"
    echo "$expected"
    echo "Got:"
    echo "$remaining"
    exit 1
fi
if [[ ! -d ".pup" ]]; then
    echo "FAIL: .pup/ should NOT be removed after dry-run"
    exit 1
fi

# Actual distclean
$PUP distclean

# Verify only source files remain after actual distclean
remaining=$(find . -type f ! -name 'test.sh' | sort)
expected=$(printf "./hello.c\n./Tupfile" | sort)
if [[ "$remaining" != "$expected" ]]; then
    echo "FAIL: Unexpected files after distclean. Expected:"
    echo "$expected"
    echo "Got:"
    echo "$remaining"
    exit 1
fi
if [[ -d ".pup" ]]; then
    echo "FAIL: .pup/ should be removed after actual distclean"
    exit 1
fi

echo "distclean_dry_run test passed"
