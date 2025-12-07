#!/bin/bash
# Test: pup can parse its own Tupfile
set -e

PUP_ROOT="$(cd "$(dirname "$PUP")" && pwd)"

# Parse pup's own Tupfile
cd "$PUP_ROOT"

echo "Parsing pup's Tupfile..."
$PUP parse -v

# Note: Building the graph requires include_rules support (for Tuprules.tup)
# which is not yet implemented. The parse test is sufficient for now.
echo "Self-hosting parse successful"
