#!/bin/bash
# Variant build test

set -e

# Create the variant directory and config (done dynamically to avoid
# confusing tup when building pup itself)
mkdir -p build
echo "# Variant config" > build/tup.config
echo "CONFIG_VARIANT=build" >> build/tup.config

# Initialize pup
$PUP init

# Build with --variant=build (use -j1 due to scheduler dependency bug)
$PUP build --variant=build -j1

# Verify outputs are in build/ directory
if [[ ! -f "build/hello.o" ]]; then
    echo "FAIL: build/hello.o not found"
    ls -la build/ 2>/dev/null || echo "build/ directory doesn't exist"
    exit 1
fi

if [[ ! -f "build/hello" ]]; then
    echo "FAIL: build/hello not found"
    ls -la build/
    exit 1
fi

# Verify outputs are NOT in source directory
if [[ -f "hello.o" ]]; then
    echo "FAIL: hello.o should not be in source directory"
    exit 1
fi

if [[ -f "hello" ]] && [[ ! -d "hello" ]]; then
    echo "FAIL: hello should not be in source directory"
    exit 1
fi

# Run the built executable
output=$(./build/hello)
if [[ "$output" != "Hello from variant build!" ]]; then
    echo "FAIL: Unexpected output: $output"
    exit 1
fi

echo "Variant build test passed: outputs in build/ directory"
exit 0
