#!/bin/bash
# Test: pup clean from inside build directory (auto-detect, no -B flag)
set -e

SOURCE_DIR="$PWD"

$PUP init
mkdir -p build
$PUP build -B build

# Verify output exists
if [[ ! -f "build/hello.o" ]]; then
    echo "FAIL: build/hello.o not created after build"
    exit 1
fi

# cd into build directory and run clean (no -B flag)
cd build
$PUP clean

# Verify output removed
if [[ -f "hello.o" ]]; then
    echo "FAIL: hello.o should be removed after clean from build dir"
    exit 1
fi

# Verify .pup/ preserved
if [[ ! -d ".pup" ]]; then
    echo "FAIL: .pup/ should be preserved by clean"
    exit 1
fi

# Verify source files untouched
cd "$SOURCE_DIR"
if [[ ! -f "hello.c" ]]; then
    echo "FAIL: hello.c should be preserved"
    exit 1
fi

# Verify complete file set
remaining=$(find . -type f ! -path './.pup/*' ! -path './build/.pup/*' ! -name 'test.sh' | sort)
expected=$(printf "./hello.c\n./Tupfile" | sort)
if [[ "$remaining" != "$expected" ]]; then
    echo "FAIL: Unexpected files after clean. Expected:"
    echo "$expected"
    echo "Got:"
    echo "$remaining"
    exit 1
fi

echo "clean_from_build_dir test passed"
