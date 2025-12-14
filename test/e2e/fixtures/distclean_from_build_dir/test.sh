#!/bin/bash
# Test: pup distclean from inside build directory (auto-detect, no -B flag)
set -e

SOURCE_DIR="$PWD"

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

# cd into build directory and run distclean (no -B flag)
cd build
$PUP distclean

# Verify output removed
if [[ -f "hello.o" ]]; then
    echo "FAIL: hello.o should be removed after distclean from build dir"
    exit 1
fi

# Verify .pup/ removed
if [[ -d ".pup" ]]; then
    echo "FAIL: .pup/ should be removed after distclean"
    exit 1
fi

# Verify source files untouched
cd "$SOURCE_DIR"
if [[ ! -f "hello.c" ]]; then
    echo "FAIL: hello.c should be preserved"
    exit 1
fi

# Verify source .pup/ preserved
if [[ ! -d ".pup" ]]; then
    echo "FAIL: source .pup/ should be preserved"
    exit 1
fi

# Verify complete file set (only source files + source .pup/)
remaining=$(find . -type f ! -path './.pup/*' ! -name 'test.sh' | sort)
expected=$(printf "./hello.c\n./Tupfile" | sort)
if [[ "$remaining" != "$expected" ]]; then
    echo "FAIL: Unexpected files after distclean. Expected:"
    echo "$expected"
    echo "Got:"
    echo "$remaining"
    exit 1
fi

echo "distclean_from_build_dir test passed"
