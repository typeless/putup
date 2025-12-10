#!/bin/bash
# Test: implicit header dependency discovery via PUP_IMPLICIT_DEPS
set -e

export PUP_IMPLICIT_DEPS=1

$PUP init
$PUP build

# First build - verify output
output1=$(./program)
if [[ "$output1" != "Version 1" ]]; then
    echo "Error: first build output wrong: $output1"
    exit 1
fi
echo "First build OK: $output1"

# No-op rebuild - should do nothing
result=$($PUP build 2>&1)
if [[ "$result" != *"0 commands"* ]] && [[ "$result" != *"Nothing to do"* ]]; then
    echo "Error: no-op rebuild should do 0 commands: $result"
    exit 1
fi
echo "No-op rebuild OK"

# Modify header (not listed in Tupfile!)
echo "#ifndef CONFIG_H" > config.h
echo "#define CONFIG_H" >> config.h
echo "#define VERSION 2" >> config.h
echo "#endif" >> config.h

# Rebuild - should recompile because header changed (implicit dep)
$PUP build

output2=$(./program)
if [[ "$output2" != "Version 2" ]]; then
    echo "Error: rebuild after header change output wrong: $output2"
    exit 1
fi

echo "Rebuild with modified header OK: $output2"
echo "Implicit dependency tracking works!"
