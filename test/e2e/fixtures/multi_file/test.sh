#!/bin/bash
# Test: multiple source files with foreach
set -e

$PUP init
$PUP build -j1  # Use -j1 until parallel scheduler respects dependencies

# Verify .o files created
for obj in main.o add.o multiply.o; do
    if [[ ! -f "./$obj" ]]; then
        echo "Error: $obj not created"
        exit 1
    fi
done

# Verify binary runs correctly
if [[ ! -x "./calc" ]]; then
    echo "Error: calc binary not created"
    exit 1
fi

output=$(./calc)
if ! echo "$output" | grep -q "add(2,3) = 5"; then
    echo "Error: add function failed"
    echo "Output: $output"
    exit 1
fi

if ! echo "$output" | grep -q "multiply(4,5) = 20"; then
    echo "Error: multiply function failed"
    echo "Output: $output"
    exit 1
fi

echo "All outputs verified"
