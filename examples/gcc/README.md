# GCC Prerequisite Libraries

Build GCC's bundled [GMP](https://gmplib.org/), [MPFR](https://www.mpfr.org/), and [MPC](http://www.multiprecision.org/mpc/) libraries using putup.

## Quick Start

```bash
# 1. Download and extract GCC (includes GMP, MPFR, MPC)
cd /path/to
wget https://gcc.gnu.org/pub/gcc/releases/gcc-15.2.0/gcc-15.2.0.tar.xz
tar xf gcc-15.2.0.tar.xz
cd gcc-15.2.0 && ./contrib/download_prerequisites && cd ..

# 2. Build (3-tree: -C = Tupfiles, -S = GCC sources, -B = output)
cd examples/gcc
putup configure --config configs/x86_64-linux.config -C . -S /path/to/gcc-15.2.0 -B build
putup -C . -S /path/to/gcc-15.2.0 -B build
```

## What Gets Built

| Library | Version | Sources | Output |
|---------|---------|---------|--------|
| GMP | 6.2.1 | 515 objects across 8 directories | `build/gmp/libgmp.a` (1.4M) |
| MPFR | 4.1.0 | 241 objects in src/ | `build/mpfr/src/libmpfr.a` (1.2M) |
| MPC | 1.2.1 | 82 objects in src/ | `build/mpc/src/libmpc.a` (267K) |

## Dependency Chain

```
GMP (no deps) → libgmp.a
    ↓
MPFR (needs GMP headers) → libmpfr.a
    ↓
MPC (needs GMP + MPFR headers) → libmpc.a
```

## Platform Configuration

Platform-specific settings live in `tup.config`, populated from a config file:

```bash
putup configure --config configs/x86_64-linux.config -C . -S /path/to/gcc-15.2.0 -B build
```

The config maps `CONFIG_<LIB>_*` variables to `#define` statements in each library's
`config.h` via the `!gen-config` bang macro. For example:

```ini
# In configs/x86_64-linux.config:
CONFIG_GMP_HAVE_ALLOCA=1
CONFIG_GMP_SIZEOF_UNSIGNED_LONG=8
```

Becomes:

```c
/* In build/gmp/config.h: */
#define HAVE_ALLOCA 1
#define SIZEOF_UNSIGNED_LONG 8
```

To support a new platform, create a new config file (e.g., `configs/aarch64-linux.config`)
and pass it to `putup configure`.

## Build Features Demonstrated

- **tup.config-driven config.h generation**: Platform defines from `tup.config` via `!gen-config`
- **Multi-directory builds**: GMP spans 8 subdirectories, each with its own Tupfile
- **Host tool generation**: GMP table generators compiled and run during the build
- **3-tree builds**: Tupfiles, GCC sources, and build output in separate directories (`-C`, `-S`, `-B`)
- **Cross-directory groups**: Subdirectory objects collected via `<objs>` groups
- **Header generation**: gmp.h generated from template via sed with `@()` substitutions
- **Inter-library dependencies**: Order-only groups ensure correct build ordering

## Files

| File | Purpose |
|------|---------|
| `Makefile.pup` | Make wrapper for putup commands |
| `Tupfile.ini` | Project root marker |
| `Tuprules.tup` | S/B convention, CFLAGS, bang macros, `!gen-config` |
| `configs/x86_64-linux.config` | Platform config (x86-64 Linux) |
| `gmp/Tupfile` | GMP config generation, table generators, top-level sources |
| `gmp/{mpn,mpz,mpq,mpf,printf,scanf,rand}/Tupfile` | Per-directory compilation |
| `mpfr/Tupfile` | MPFR config generation |
| `mpfr/src/Tupfile` | MPFR source compilation |
| `mpc/Tupfile` | MPC config generation |
| `mpc/src/Tupfile` | MPC source compilation |

## Notes

- Uses generic C implementations only (no assembly) -- equivalent to `--disable-assembly`
- Requires: gcc (or compatible C compiler)
- 1708 build commands total, completes in ~10 seconds on a modern machine
- The GMP build compiles 515 source files using mini-gmp-based generators for lookup tables
- MPFR's `mpfr-mini-gmp.c` compiles to empty when not using mini-gmp mode
