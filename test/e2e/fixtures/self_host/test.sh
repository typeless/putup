#!/bin/bash
# Test: putup can parse its own Tupfile
set -e

# Get the putup source root (one level up from build directory)
PUP_ROOT="$(cd "$(dirname "$PUP")/.." && pwd)"

# Parse putup's own Tupfile
cd "$PUP_ROOT"

echo "Parsing putup's Tupfile..."
$PUP parse -v

# Note: Building the graph requires include_rules support (for Tuprules.tup)
# which is not yet implemented. The parse test is sufficient for now.
echo "Self-hosting parse successful"
