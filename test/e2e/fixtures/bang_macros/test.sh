#!/bin/bash
# Test: bang macros with Tuprules.tup
set -e

$PUP init
$PUP build -j1  # Use -j1 until parallel scheduler respects dependencies

if [[ ! -x "./greeter" ]]; then
    echo "Error: greeter binary not created"
    exit 1
fi

output=$(./greeter)
expected="Hello from bang macros!"
if [[ "$output" != "$expected" ]]; then
    echo "Error: unexpected output"
    echo "Expected: $expected"
    echo "Got: $output"
    exit 1
fi

echo "Bang macros working: $output"
