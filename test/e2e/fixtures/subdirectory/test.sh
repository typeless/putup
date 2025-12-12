#!/bin/bash
# Subdirectory build test
# Tests incremental builds with Tupfiles in subdirectories

set -e

# Initialize pup
$PUP init

# First build
$PUP build

# Verify output exists in subdirectory
if [[ ! -f "src/lib/program" ]]; then
    echo "FAIL: src/lib/program not found"
    ls -la src/lib/ 2>/dev/null || echo "src/lib/ doesn't exist"
    exit 1
fi

# Run the built executable
output=$(./src/lib/program)
if [[ "$output" != "Config value: 42" ]]; then
    echo "FAIL: Unexpected output: $output"
    exit 1
fi
echo "First build OK: $output"

# Second build should be no-op
rebuild_output=$($PUP build 2>&1)
if ! echo "$rebuild_output" | grep -q "Nothing to do"; then
    echo "FAIL: Second build should be no-op, got:"
    echo "$rebuild_output"
    exit 1
fi
echo "No-op rebuild OK"

# Modify header in different directory and rebuild
echo '#define CONFIG_VALUE 100' > include/config.h

rebuild_output=$($PUP build 2>&1)
if echo "$rebuild_output" | grep -q "Nothing to do"; then
    echo "FAIL: Build should rebuild after header change"
    exit 1
fi

# Verify new output
output=$(./src/lib/program)
if [[ "$output" != "Config value: 100" ]]; then
    echo "FAIL: Unexpected output after header change: $output"
    exit 1
fi
echo "Cross-directory header rebuild OK: $output"

echo "Subdirectory build test passed"
exit 0
