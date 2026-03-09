# BSP — Board Support Package Example

A complete cross-compiler toolchain built from source using putup.
Demonstrates multi-tarball, multi-package builds with 3-tree mode (`-C`, `-S`, `-B`).

## Packages

```
gcc/                         GCC tarball group (gcc-15.2.0)
  gcc/                       Compiler: cc1, cc1plus, xgcc, xg++, cpp, collect2, ...
  gmp/                       Math library: libgmp.a
  mpfr/                      Multi-precision floats: libmpfr.a
  mpc/                       Complex arithmetic: libmpc.a
  libiberty/                 Utility library: libiberty.a
  libcpp/                    C preprocessor: libcpp.a
  libdecnumber/              Decimal floating-point: libdecnumber.a
  libbacktrace/              Stack unwinding: libbacktrace.a
  libcody/                   C++ modules protocol: libcody.a

binutils/                    binutils tarball group (binutils-2.44)
                             Cross-assembler (as) + cross-archiver (ar)
```

## Source Tree Assembly

Each top-level config directory maps to one extracted tarball.
Assemble the source tree by extracting and renaming:

```bash
mkdir -p source-root
# GCC (includes GMP, MPFR, MPC via download_prerequisites)
tar xf gcc-15.2.0.tar.xz
mv gcc-15.2.0 source-root/gcc
cd source-root/gcc && ./contrib/download_prerequisites && cd ../..

# binutils
tar xf binutils-2.44.tar.xz
mv binutils-2.44 source-root/binutils
```

The config tree (`-C examples/bsp/`) mirrors the source tree (`-S source-root/`):

```
examples/bsp/       (config tree)     source-root/        (source tree)
├── gcc/                              ├── gcc/
│   ├── gcc/                          │   ├── gcc/
│   ├── gmp/                          │   ├── gmp/
│   └── ...                           │   └── ...
└── binutils/                         └── binutils/
```

## Quick Start

```bash
cd examples/bsp

# Download source (or assemble manually — see above)
scripts/download-source.sh ../../source-root

# Configure + build (native x86-64 Linux)
putup configure --config configs/x86_64-linux.config \
    -C . -S ../../source-root -B ../../build-gcc
putup -C . -S ../../source-root -B ../../build-gcc -j$(nproc)

# Cross-compile (macOS host → x86-64 Linux target)
putup configure --config configs/darwin-x86_64-linux.config \
    -C . -S ../../source-root -B ../../build-gcc
putup -C . -S ../../source-root -B ../../build-gcc -j$(nproc)
```

## Scoped Builds

Build individual packages without rebuilding everything:

```bash
# Just binutils
putup -C . -S ../../source-root -B ../../build-gcc binutils/

# Just the GCC compiler (assuming libraries are already built)
putup -C . -S ../../source-root -B ../../build-gcc gcc/gcc/

# Just GMP
putup -C . -S ../../source-root -B ../../build-gcc gcc/gmp/
```

## Standalone Mode

Each package group is independently buildable. The `?=` defaults in each
package's `Tuprules.tup` provide flat paths when there's no BSP root above:

```bash
# Build just the GCC group (standalone)
cd gcc
putup configure -S /path/to/gcc-15.2.0 -B /path/to/build
putup -S /path/to/gcc-15.2.0 -B /path/to/build
```

## Build Commands

| Command | Description |
|---------|-------------|
| `scripts/download-source.sh [SRCDIR]` | Download GCC + binutils tarballs |
| `putup configure --config configs/<platform>.config -C . -S <src> -B <build>` | Configure |
| `putup -C . -S <src> -B <build> -j$(nproc)` | Build |
| `putup clean -C . -S <src> -B <build>` | Clean build artifacts |
| `putup distclean -C . -S <src> -B <build>` | Full clean (remove build directory) |

**Platform configs:** `configs/x86_64-linux.config`, `configs/darwin-x86_64-linux.config`

**Environment variables for `download-source.sh`:**

| Variable | Default | Description |
|----------|---------|-------------|
| `GCC_VERSION` | `15.2.0` | GCC tarball version |
| `BINUTILS_VERSION` | `2.44` | binutils tarball version |

**Config variables (in platform config):**

| Variable | Default | Description |
|----------|---------|-------------|
| `CONFIG_MPN_CPU` | `generic` | GMP assembly target (`generic`, `x86_64`, `x86_64/core2`) |

## Architecture

- **Root `Tuprules.tup`** uses `=` (force-set) for all DIR variables (e.g. `GCC_DIR = gcc/gcc`, `BINUTILS_DIR = binutils`)
- **Group `Tuprules.tup`** uses `?=` (default) — overridden in BSP mode, effective standalone
- **Package `Tuprules.tup`** uses `?=` for toolchain vars — overridden by both root and group
- `include_rules` processes root-first; BSP root wins over group defaults

## Known Limitations

- putup `create_directories` crashes on symlinks in source tree
- busybox and helloworld live at `examples/` (outside BSP scan tree)
