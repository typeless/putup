#!/bin/bash
# Test: .pupignore ignores directories during Tupfile discovery
set -e

# Initialize and get graph output with verbose to see discovered dirs
$PUP init

# Use 'export graph --summary -v' to get verbose graph output
output=$($PUP export graph --summary -v 2>&1)

# Should find 2 directories (. and src), NOT 3 (ignored/ should be skipped)
if echo "$output" | grep -q "Found 2 directories"; then
    echo "Correct: Found 2 directories (ignored/ was skipped)"
else
    echo "Error: Expected 'Found 2 directories'"
    echo "Output: $output"
    exit 1
fi

# Verify by checking command count (should be 2, not 3)
if echo "$output" | grep -q "Commands: 2"; then
    echo "Correct: 2 commands (ignored/Tupfile was not parsed)"
else
    echo "Error: Expected 2 commands"
    echo "Output: $output"
    exit 1
fi
