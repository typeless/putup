#!/bin/bash
# Test: output groups {objs}
set -e

$PUP init
$PUP -j1

# Verify all object files were created
for f in main.o math.o util.o; do
    if [ ! -f "$f" ]; then
        echo "FAIL: Missing $f"
        exit 1
    fi
done

# Verify executable was created and works
if [ ! -f calculator ]; then
    echo "FAIL: Missing calculator executable"
    exit 1
fi

OUTPUT=$(./calculator)
if ! echo "$OUTPUT" | grep -q "3 + 4 = 7"; then
    echo "FAIL: Expected '3 + 4 = 7'"
    echo "Got: $OUTPUT"
    exit 1
fi
if ! echo "$OUTPUT" | grep -q "3 \\* 4 = 12"; then
    echo "FAIL: Expected '3 * 4 = 12'"
    echo "Got: $OUTPUT"
    exit 1
fi

echo "Groups test passed: $OUTPUT"
