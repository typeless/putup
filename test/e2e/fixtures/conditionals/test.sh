#!/bin/bash
# Test: conditionals (ifdef, ifndef, ifeq, ifneq)
set -e

# Test 1: Default build (USE_GREETING not set, NO_EXTRA not set)
# Expected: "Goodbye, World!" and "Extra enabled"
$PUP configure
$PUP -j1

./program > output.txt
if ! grep -q "Goodbye, World!" output.txt; then
    echo "FAIL: Expected 'Goodbye, World!' (USE_GREETING not defined)"
    exit 1
fi
if ! grep -q "Extra enabled" output.txt; then
    echo "FAIL: Expected 'Extra enabled' (NO_EXTRA not defined)"
    exit 1
fi
echo "Default build OK"

# Clean and rebuild with USE_GREETING defined
rm -rf .pup tup.config program *.o

# Create a modified Tupfile with USE_GREETING defined
cat > Tupfile << 'EOF'
CC = gcc
CFLAGS = -Wall
USE_GREETING = y

ifdef USE_GREETING
CFLAGS += -DUSE_GREETING
endif

ifndef NO_EXTRA
CFLAGS += -DUSE_EXTRA
endif

ifeq ($(CC),gcc)
CFLAGS += -Wextra
endif

ifneq ($(CC),clang)
CFLAGS += -Wpedantic
endif

: main.c |> $(CC) $(CFLAGS) -o %o %f |> program
EOF

$PUP configure
$PUP -j1

./program > output.txt
if ! grep -q "Hello, World!" output.txt; then
    echo "FAIL: Expected 'Hello, World!' (USE_GREETING defined)"
    exit 1
fi
echo "USE_GREETING build OK"

# Test 3: With NO_EXTRA defined (should disable extra)
rm -rf .pup tup.config program *.o

cat > Tupfile << 'EOF'
CC = gcc
CFLAGS = -Wall
NO_EXTRA = y

ifdef USE_GREETING
CFLAGS += -DUSE_GREETING
endif

ifndef NO_EXTRA
CFLAGS += -DUSE_EXTRA
endif

: main.c |> $(CC) $(CFLAGS) -o %o %f |> program
EOF

$PUP configure
$PUP -j1

./program > output.txt
if grep -q "Extra enabled" output.txt; then
    echo "FAIL: Expected no 'Extra enabled' (NO_EXTRA defined)"
    exit 1
fi
echo "NO_EXTRA build OK"

echo "All conditional tests passed"
