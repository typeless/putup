#!/bin/bash
# Test: pup clean -n (dry-run) doesn't actually remove files
set -e

$PUP init
$PUP build

# Verify output exists
if [[ ! -f "hello.o" ]]; then
    echo "FAIL: hello.o not created after build"
    exit 1
fi

# Dry-run clean
output=$($PUP clean -n 2>&1)

# Verify "Would remove" in output
if ! echo "$output" | grep -q "Would remove"; then
    echo "FAIL: Dry-run should report 'Would remove', got:"
    echo "$output"
    exit 1
fi

# Verify all files still exist after dry-run (including output)
remaining=$(find . -type f ! -path './.pup/*' ! -name 'test.sh' | sort)
expected=$(printf "./hello.c\n./hello.o\n./Tupfile" | sort)
if [[ "$remaining" != "$expected" ]]; then
    echo "FAIL: Dry-run should not remove files. Expected:"
    echo "$expected"
    echo "Got:"
    echo "$remaining"
    exit 1
fi

# Actual clean
$PUP clean

# Verify only source files remain after actual clean
remaining=$(find . -type f ! -path './.pup/*' ! -name 'test.sh' | sort)
expected=$(printf "./hello.c\n./Tupfile" | sort)
if [[ "$remaining" != "$expected" ]]; then
    echo "FAIL: Unexpected files after clean. Expected:"
    echo "$expected"
    echo "Got:"
    echo "$remaining"
    exit 1
fi

echo "clean_dry_run test passed"
