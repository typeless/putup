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

# Assemble source tree (or use: make -f Makefile.pup download-source)
# ... see above ...

# Build (macOS host → x86-64 Linux target)
make -f Makefile.pup SRCDIR=../../source-root PLATFORM=darwin-x86_64-linux HOST=darwin

# Or directly with putup:
putup configure --config configs/darwin-x86_64-linux.config \
    -C . -S ../../source-root -B ../../build-gcc
putup -C . -S ../../source-root -B ../../build-gcc -j8
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

## Makefile Targets

| Target | Description |
|--------|-------------|
| `make -f Makefile.pup` | Full build (resolve-mpn + configure + build) |
| `make -f Makefile.pup configure` | Configure only (no build) |
| `make -f Makefile.pup download-source` | Download GCC + binutils tarballs into `SRCDIR` |
| `make -f Makefile.pup clean` | Clean build artifacts |
| `make -f Makefile.pup distclean` | Full clean (remove build directory) |
| `make -f Makefile.pup multi` | Multi-variant parallel build for `PLATFORMS` |

**Variables:**

| Variable | Default | Description |
|----------|---------|-------------|
| `PLATFORM` | `x86_64-linux` | Config file selector (`configs/$(PLATFORM).config`) |
| `HOST` | `linux` | Host OS (overlays `configs/host-$(HOST)/` if not linux) |
| `SRCDIR` | `../../source-root` | Assembled source tree path |
| `BUILD` | `../../build-gcc` | Build output directory |
| `MPN_CPU` | `generic` | GMP assembly target (`generic`, `x86_64`, `x86_64/core2`) |
| `PUTUP` | `putup` | Path to putup binary (override for CI) |
| `GCC_VERSION` | `15.2.0` | GCC tarball version for `download-source` |
| `BINUTILS_VERSION` | `2.44` | binutils tarball version for `download-source` |

## Architecture

- **Root `Tuprules.tup`** uses `=` (force-set) for all DIR variables (e.g. `GCC_DIR = gcc/gcc`, `BINUTILS_DIR = binutils`)
- **Group `Tuprules.tup`** uses `?=` (default) — overridden in BSP mode, effective standalone
- **Package `Tuprules.tup`** uses `?=` for toolchain vars — overridden by both root and group
- `include_rules` processes root-first; BSP root wins over group defaults

## Known Limitations

- putup `create_directories` crashes on symlinks in source tree
- busybox and helloworld live at `examples/` (outside BSP scan tree)
