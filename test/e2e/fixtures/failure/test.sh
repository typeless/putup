#!/bin/bash
# Test: command failure handling
set -e

$PUP init

# Build should fail
if $PUP -j1 2>&1; then
    echo "FAIL: Build should have failed"
    exit 1
fi

# Verify no output file was created
if [ -f bad.o ]; then
    echo "FAIL: bad.o should not exist after failed build"
    exit 1
fi

echo "Failure test passed: Build correctly reported failure"
