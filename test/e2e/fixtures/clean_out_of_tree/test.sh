#!/bin/bash
# Test: pup clean -B (out-of-tree build)
set -e

$PUP init
mkdir -p build
$PUP build -B build

# Verify output in build directory
if [[ ! -f "build/hello.o" ]]; then
    echo "FAIL: build/hello.o not created after build"
    exit 1
fi

# Verify output NOT in source directory
if [[ -f "hello.o" ]]; then
    echo "FAIL: hello.o should not be in source directory"
    exit 1
fi

# Clean
$PUP clean -B build

# Verify only expected files remain (excluding .pup/ internals and test.sh)
remaining=$(find . -type f ! -path './.pup/*' ! -path './build/.pup/*' ! -name 'test.sh' | sort)
expected=$(printf "./hello.c\n./Tupfile" | sort)
if [[ "$remaining" != "$expected" ]]; then
    echo "FAIL: Unexpected files after clean. Expected:"
    echo "$expected"
    echo "Got:"
    echo "$remaining"
    exit 1
fi

# Verify build/.pup/ preserved
if [[ ! -d "build/.pup" ]]; then
    echo "FAIL: build/.pup/ should be preserved by clean"
    exit 1
fi

# Rebuild after clean
$PUP build -B build
if [[ ! -f "build/hello.o" ]]; then
    echo "FAIL: build/hello.o not created after rebuild"
    exit 1
fi

# Clean again
$PUP clean -B build
if [[ -f "build/hello.o" ]]; then
    echo "FAIL: build/hello.o should be removed after second clean"
    exit 1
fi

echo "clean_out_of_tree test passed"
