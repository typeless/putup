#!/bin/sh
# download-source.sh — Download and assemble GCC + binutils source trees
#
# Usage: scripts/download-source.sh [SRCDIR]
#
# Environment:
#   GCC_VERSION      GCC version to download (default: 15.2.0)
#   BINUTILS_VERSION binutils version to download (default: 2.44)

set -e

GCC_VERSION=${GCC_VERSION:-15.2.0}
BINUTILS_VERSION=${BINUTILS_VERSION:-2.44}
SRCDIR=${1:-../../source-root}

# Download binutils
if [ ! -d "$SRCDIR/binutils" ]; then
    echo "Downloading binutils $BINUTILS_VERSION..."
    mkdir -p "$SRCDIR/binutils"
    curl -L "https://ftp.gnu.org/gnu/binutils/binutils-$BINUTILS_VERSION.tar.xz" \
        | tar xJ --strip-components=1 -C "$SRCDIR/binutils"
else
    echo "$SRCDIR/binutils already exists"
fi

# Download GCC + prerequisites
if [ ! -d "$SRCDIR/gcc" ]; then
    echo "Downloading GCC $GCC_VERSION..."
    mkdir -p "$SRCDIR/gcc"
    curl -L "https://ftp.gnu.org/gnu/gcc/gcc-$GCC_VERSION/gcc-$GCC_VERSION.tar.xz" \
        | tar xJ --strip-components=1 -C "$SRCDIR/gcc"
    cd "$SRCDIR/gcc" && ./contrib/download_prerequisites
else
    echo "$SRCDIR/gcc already exists"
fi
