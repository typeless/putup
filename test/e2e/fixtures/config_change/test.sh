#!/bin/bash
# Test: tup.config changes trigger rebuild
set -e

# Create variant directory with initial config
mkdir -p build
echo "CONFIG_OPT=1" > build/tup.config

$PUP init
$PUP build -B build

# Verify output
output=$(./build/program)
if [[ "$output" != "Optimization: 1" ]]; then
    echo "FAIL: Expected 'Optimization: 1', got: $output"
    exit 1
fi
echo "First build OK: $output"

# Second build should be no-op
rebuild_output=$($PUP build -B build 2>&1)
if ! echo "$rebuild_output" | grep -q "Nothing to do"; then
    echo "FAIL: Second build should be no-op, got:"
    echo "$rebuild_output"
    exit 1
fi
echo "No-op rebuild OK"

# Change config value
echo "CONFIG_OPT=2" > build/tup.config

# Third build SHOULD rebuild after config change
rebuild_output=$($PUP build -B build 2>&1)
if echo "$rebuild_output" | grep -q "Nothing to do"; then
    echo "FAIL: Build should rebuild after config change"
    exit 1
fi
echo "Config change detected OK"

# Verify new output
output=$(./build/program)
if [[ "$output" != "Optimization: 2" ]]; then
    echo "FAIL: Expected 'Optimization: 2', got: $output"
    exit 1
fi
echo "Rebuild OK: $output"

# Fourth build should stabilize
rebuild_output=$($PUP build -B build 2>&1)
if ! echo "$rebuild_output" | grep -q "Nothing to do"; then
    echo "FAIL: Build should stabilize, got:"
    echo "$rebuild_output"
    exit 1
fi
echo "Stabilization OK"

echo "Config change test passed"
