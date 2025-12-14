#!/bin/bash
# Test: pup distclean -B (out-of-tree build)
set -e

$PUP init
mkdir -p build
$PUP build -B build

# Verify setup
if [[ ! -f "build/hello.o" ]]; then
    echo "FAIL: build/hello.o not created after build"
    exit 1
fi
if [[ ! -d "build/.pup" ]]; then
    echo "FAIL: build/.pup/ not created after build"
    exit 1
fi

# Distclean
$PUP distclean -B build

# Verify only source files remain (build dir cleaned, source .pup/ untouched)
remaining=$(find . -type f ! -path './.pup/*' ! -name 'test.sh' | sort)
expected=$(printf "./hello.c\n./Tupfile" | sort)
if [[ "$remaining" != "$expected" ]]; then
    echo "FAIL: Unexpected files after distclean. Expected:"
    echo "$expected"
    echo "Got:"
    echo "$remaining"
    exit 1
fi

# Verify build/.pup/ removed
if [[ -d "build/.pup" ]]; then
    echo "FAIL: build/.pup/ should be removed after distclean"
    exit 1
fi

# Verify source .pup/ preserved (only build dir was distcleaned)
if [[ ! -d ".pup" ]]; then
    echo "FAIL: source .pup/ should be preserved"
    exit 1
fi

# Recovery: build again after distclean
mkdir -p build
$PUP build -B build

if [[ ! -f "build/hello.o" ]]; then
    echo "FAIL: build/hello.o not created after recovery build"
    exit 1
fi
if [[ ! -d "build/.pup" ]]; then
    echo "FAIL: build/.pup/ not created after recovery build"
    exit 1
fi

# Second distclean cycle
$PUP distclean -B build

remaining=$(find . -type f ! -path './.pup/*' ! -name 'test.sh' | sort)
expected=$(printf "./hello.c\n./Tupfile" | sort)
if [[ "$remaining" != "$expected" ]]; then
    echo "FAIL: Unexpected files after second distclean. Expected:"
    echo "$expected"
    echo "Got:"
    echo "$remaining"
    exit 1
fi

echo "distclean_out_of_tree test passed"
