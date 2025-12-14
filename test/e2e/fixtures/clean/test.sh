#!/bin/bash
# Test: pup clean removes generated files but preserves .pup/
set -e

$PUP init
$PUP build

# Verify output exists
if [[ ! -f "hello.o" ]]; then
    echo "FAIL: hello.o not created after build"
    exit 1
fi

# Clean
$PUP clean

# Verify only expected files remain (excluding .pup/ internals and test.sh)
remaining=$(find . -type f ! -path './.pup/*' ! -name 'test.sh' | sort)
expected=$(printf "./hello.c\n./Tupfile" | sort)
if [[ "$remaining" != "$expected" ]]; then
    echo "FAIL: Unexpected files after clean. Expected:"
    echo "$expected"
    echo "Got:"
    echo "$remaining"
    exit 1
fi

# Verify .pup/ preserved
if [[ ! -d ".pup" ]]; then
    echo "FAIL: .pup/ should be preserved by clean"
    exit 1
fi

# Second clean should report 0 files
output=$($PUP clean 2>&1)
if ! echo "$output" | grep -q "Removed 0 files"; then
    echo "FAIL: Second clean should report 0 files, got:"
    echo "$output"
    exit 1
fi

# Rebuild after clean (index preserved, should work)
$PUP build
if [[ ! -f "hello.o" ]]; then
    echo "FAIL: hello.o not created after rebuild"
    exit 1
fi

# Clean again
$PUP clean
if [[ -f "hello.o" ]]; then
    echo "FAIL: hello.o should be removed after second clean"
    exit 1
fi

echo "clean test passed"
