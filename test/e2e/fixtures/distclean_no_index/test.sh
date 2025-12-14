#!/bin/bash
# Test: pup distclean with no prior build (no index, no .pup/)
set -e

$PUP init
# Don't build - no index exists

# distclean should handle gracefully
output=$($PUP distclean 2>&1)

# Verify only source files remain (no .pup/)
remaining=$(find . -type f ! -name 'test.sh' | sort)
expected=$(printf "./hello.c\n./Tupfile" | sort)
if [[ "$remaining" != "$expected" ]]; then
    echo "FAIL: Unexpected files after distclean. Expected:"
    echo "$expected"
    echo "Got:"
    echo "$remaining"
    exit 1
fi

# Verify .pup/ removed
if [[ -d ".pup" ]]; then
    echo "FAIL: .pup/ should be removed by distclean"
    exit 1
fi

# Verify "Project reset complete" message
if ! echo "$output" | grep -q "Project reset complete"; then
    echo "FAIL: Should report 'Project reset complete', got:"
    echo "$output"
    exit 1
fi

echo "distclean_no_index test passed"
