#!/bin/bash
# Variant build test with symlinked tup.config
# Tests that symlinked config files don't break incremental builds

set -e

# Create config in separate location (like real projects do)
mkdir -p configs build
echo "# Symlinked variant config" > configs/build.config
echo "CONFIG_VARIANT=build" >> configs/build.config

# Symlink tup.config to the config file
ln -s ../configs/build.config build/tup.config

# Verify symlink was created
if [[ ! -L "build/tup.config" ]]; then
    echo "FAIL: build/tup.config should be a symlink"
    exit 1
fi

# Initialize pup
$PUP init

# First build
$PUP build --variant=build

# Verify output exists
if [[ ! -f "build/hello" ]]; then
    echo "FAIL: build/hello not found"
    ls -la build/
    exit 1
fi

# Run the built executable
output=$(./build/hello)
if [[ "$output" != "Hello from symlinked variant!" ]]; then
    echo "FAIL: Unexpected output: $output"
    exit 1
fi

# Second build should be no-op (this is the key test)
# If symlink resolution is broken, pup will think tup.config changed
rebuild_output=$($PUP build --variant=build 2>&1)
if ! echo "$rebuild_output" | grep -q "Nothing to do"; then
    echo "FAIL: Second build should be no-op with symlinked tup.config, got:"
    echo "$rebuild_output"
    exit 1
fi

echo "Symlinked tup.config test passed"
exit 0
