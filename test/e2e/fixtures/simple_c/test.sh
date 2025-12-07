#!/bin/bash
# Test: simple C compilation
set -e

# Initialize and build
$PUP init
$PUP build

# Verify output exists and runs
if [[ ! -x "./hello" ]]; then
    echo "Error: hello binary not created"
    exit 1
fi

output=$(./hello)
if [[ "$output" != "Hello from pup!" ]]; then
    echo "Error: unexpected output: $output"
    exit 1
fi

echo "Output verified: $output"
