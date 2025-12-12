#!/bin/bash
# Out-of-tree build test using -B flag
# Tests incremental builds with separate source and build directories

set -e

# Initialize pup in source directory
$PUP init

# Create build directory (separate from source)
mkdir -p build

# First build with -B flag
$PUP build -B build

# Verify output exists in build directory, not source
if [[ ! -f "build/hello" ]]; then
    echo "FAIL: build/hello not found"
    ls -la build/ 2>/dev/null || echo "build/ directory doesn't exist"
    exit 1
fi

if [[ -f "hello" ]] && [[ ! -d "hello" ]]; then
    echo "FAIL: hello should not be in source directory"
    exit 1
fi

# Run the built executable
output=$(./build/hello)
if [[ "$output" != "Hello from out-of-tree build!" ]]; then
    echo "FAIL: Unexpected output: $output"
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

# Modify header and rebuild
echo '#define MESSAGE "Modified out-of-tree!"' > hello.h

rebuild_output=$($PUP build -B build 2>&1)
if echo "$rebuild_output" | grep -q "Nothing to do"; then
    echo "FAIL: Build should rebuild after header change"
    exit 1
fi

# Verify new output
output=$(./build/hello)
if [[ "$output" != "Modified out-of-tree!" ]]; then
    echo "FAIL: Unexpected output after header change: $output"
    exit 1
fi
echo "Header rebuild OK: $output"

echo "Out-of-tree build test passed"
exit 0
