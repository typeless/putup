#!/bin/bash
# Test: Tupfile command string changes trigger rebuild
set -e

$PUP init
$PUP build

# Verify output
output=$(./program)
if [[ "$output" != "Version: 1" ]]; then
    echo "FAIL: Expected 'Version: 1', got: $output"
    exit 1
fi
echo "First build OK: $output"

# Second build should be no-op
rebuild_output=$($PUP build 2>&1)
if ! echo "$rebuild_output" | grep -q "Nothing to do"; then
    echo "FAIL: Second build should be no-op, got:"
    echo "$rebuild_output"
    exit 1
fi
echo "No-op rebuild OK"

# Change Tupfile command (without touching source files)
cat > Tupfile << 'EOF'
: main.c |> gcc -DVERSION=2 -o %o %f |> program
EOF

# Third build SHOULD rebuild
rebuild_output=$($PUP build 2>&1)
if echo "$rebuild_output" | grep -q "Nothing to do"; then
    echo "FAIL: Build should rebuild after Tupfile change"
    exit 1
fi
echo "Tupfile change detected OK"

# Verify new output
output=$(./program)
if [[ "$output" != "Version: 2" ]]; then
    echo "FAIL: Expected 'Version: 2', got: $output"
    exit 1
fi
echo "Rebuild OK: $output"

# Fourth build should stabilize
rebuild_output=$($PUP build 2>&1)
if ! echo "$rebuild_output" | grep -q "Nothing to do"; then
    echo "FAIL: Build should stabilize, got:"
    echo "$rebuild_output"
    exit 1
fi
echo "Stabilization OK"

echo "=== Test Part 2: Header + Tupfile change together ==="

# Add a header file
cat > config.h << 'EOF'
#define CONFIG_VALUE 100
EOF

# Update main.c to use the header
cat > main.c << 'EOF'
#include <stdio.h>
#include "config.h"
#ifndef VERSION
#define VERSION 0
#endif
int main() { printf("Version: %d, Config: %d\n", VERSION, CONFIG_VALUE); return 0; }
EOF

# Update Tupfile to use -MD for implicit deps
cat > Tupfile << 'EOF'
: main.c |> gcc -MD -DVERSION=3 -o %o %f |> program
EOF

# Build with implicit deps
$PUP build
output=$(./program)
if [[ "$output" != "Version: 3, Config: 100" ]]; then
    echo "FAIL: Expected 'Version: 3, Config: 100', got: $output"
    exit 1
fi
echo "Implicit deps build OK: $output"

# Stabilization check
rebuild_output=$($PUP build 2>&1)
if ! echo "$rebuild_output" | grep -q "Nothing to do"; then
    echo "FAIL: Build should stabilize after implicit deps build, got:"
    echo "$rebuild_output"
    exit 1
fi
echo "Implicit deps stabilization OK"

# Now the critical test: change BOTH header AND Tupfile command
cat > config.h << 'EOF'
#define CONFIG_VALUE 200
EOF

cat > Tupfile << 'EOF'
: main.c |> gcc -MD -DVERSION=4 -o %o %f |> program
EOF

# This build MUST rebuild (header changed + command changed)
# BUG: expand_implicit_deps() might skip because old command not in new graph
rebuild_output=$($PUP build 2>&1)
if echo "$rebuild_output" | grep -q "Nothing to do"; then
    echo "FAIL: Build should rebuild after header+Tupfile change"
    exit 1
fi
echo "Header + Tupfile change detected OK"

output=$(./program)
if [[ "$output" != "Version: 4, Config: 200" ]]; then
    echo "FAIL: Expected 'Version: 4, Config: 200', got: $output"
    exit 1
fi
echo "Header + Tupfile rebuild OK: $output"

# Final stabilization
rebuild_output=$($PUP build 2>&1)
if ! echo "$rebuild_output" | grep -q "Nothing to do"; then
    echo "FAIL: Build should stabilize after header+Tupfile rebuild, got:"
    echo "$rebuild_output"
    exit 1
fi
echo "Final stabilization OK"

echo "Tupfile change test passed"
