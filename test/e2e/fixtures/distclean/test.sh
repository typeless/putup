#!/bin/bash
# Test: pup distclean removes outputs, .pup/, and tup.config
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

# Distclean
output=$($PUP distclean 2>&1)

# Verify only source files remain (no outputs, no .pup/, no tup.config)
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
    echo "FAIL: .pup/ should be removed after distclean"
    exit 1
fi

# Verify "Project reset complete" message
if ! echo "$output" | grep -q "Project reset complete"; then
    echo "FAIL: Should report 'Project reset complete', got:"
    echo "$output"
    exit 1
fi

# Recovery: init and build again after distclean
$PUP init
$PUP build

if [[ ! -f "hello.o" ]]; then
    echo "FAIL: hello.o not created after recovery build"
    exit 1
fi
if [[ ! -d ".pup" ]]; then
    echo "FAIL: .pup/ not created after recovery build"
    exit 1
fi

# Second distclean cycle
$PUP distclean

remaining=$(find . -type f ! -name 'test.sh' | sort)
expected=$(printf "./hello.c\n./Tupfile" | sort)
if [[ "$remaining" != "$expected" ]]; then
    echo "FAIL: Unexpected files after second distclean. Expected:"
    echo "$expected"
    echo "Got:"
    echo "$remaining"
    exit 1
fi

echo "distclean test passed"
