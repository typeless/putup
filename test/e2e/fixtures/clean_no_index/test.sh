#!/bin/bash
# Test: pup clean with no prior build (no index)
set -e

$PUP init
# Don't build - no index exists

output=$($PUP clean 2>&1)

# Verify "Nothing to clean" message
if ! echo "$output" | grep -q "Nothing to clean"; then
    echo "FAIL: Should report 'Nothing to clean', got:"
    echo "$output"
    exit 1
fi

# Verify all source files still exist (clean is no-op without index)
remaining=$(find . -type f ! -path './.pup/*' ! -name 'test.sh' | sort)
expected=$(printf "./hello.c\n./Tupfile" | sort)
if [[ "$remaining" != "$expected" ]]; then
    echo "FAIL: Files should be unchanged. Expected:"
    echo "$expected"
    echo "Got:"
    echo "$remaining"
    exit 1
fi

# Verify .pup/ still exists
if [[ ! -d ".pup" ]]; then
    echo "FAIL: .pup/ should still exist"
    exit 1
fi

echo "clean_no_index test passed"
