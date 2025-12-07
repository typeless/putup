#!/bin/bash
# Test: basic build with header dependency
# NOTE: Full incremental rebuild testing is pending index persistence implementation
set -e

$PUP init
$PUP build

# First build - verify output
output1=$(./program)
if [[ "$output1" != "Value is 42" ]]; then
    echo "Error: first build output wrong: $output1"
    exit 1
fi
echo "First build OK: $output1"

# Modify header
echo "#define VALUE 100" > value.h

# Rebuild - should recompile with new value
$PUP build

output2=$(./program)
if [[ "$output2" != "Value is 100" ]]; then
    echo "Error: rebuild output wrong: $output2"
    exit 1
fi

echo "Rebuild with modified header OK: $output2"
