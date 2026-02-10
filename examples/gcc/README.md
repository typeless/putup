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
make -f Makefile.pup SRCDIR=/path/to/gcc-15.2.0 BUILD=../build-gcc
```

Or directly (after running resolve-mpn):

```bash
scripts/resolve-mpn.sh generic /path/to/gcc-15.2.0/gmp/mpn > gmp/mpn/tup.config
putup configure --config configs/x86_64-linux.config -C . -S /path/to/gcc-15.2.0 -B build
putup -C . -S /path/to/gcc-15.2.0 -B build
```

## What Gets Built

| Library | Version | Output |
|---------|---------|--------|
| GMP | 6.2.1 | `build/gmp/libgmp.a` |
| MPFR | 4.1.0 | `build/mpfr/src/libmpfr.a` |
| MPC | 1.2.1 | `build/mpc/src/libmpc.a` |

## Dependency Chain

```
GMP (no deps) → libgmp.a
    ↓
MPFR (needs GMP headers) → libmpfr.a
    ↓
MPC (needs GMP + MPFR headers) → libmpc.a
```

## Per-Component Configuration

Each library has its own `tup.config` with prefix-free entries. The directory scope
provides the namespace via scoped config merging:

```
configs/x86_64-linux.config   → Root: CC, AR, HOSTCC (toolchain)
gmp/tup.config                → GMP: HAVE_ALLOCA, SIZEOF_UNSIGNED_LONG, ...
mpfr/tup.config               → MPFR: HAVE_ALLOCA, HAVE_VA_COPY, ...
mpc/tup.config                → MPC: HAVE_INTTYPES_H, HAVE_LOCALECONV, ...
```

`putup configure --config` installs the root config and copies subdir configs to the
build tree in a single invocation. Scoped config merging then combines them:

- `@(CC)` in gmp/ resolves to `gcc` (from root tup.config)
- `@(HAVE_ALLOCA)` in gmp/ resolves to `1` (from gmp/tup.config)
- `@(HAVE_ALLOCA)` in mpfr/ resolves to `1` (from mpfr/tup.config)

Each library's `!gen-config` reads the raw `$(B)/tup.config` to generate `config.h`,
stripping the 7-char `CONFIG_` prefix from each entry.

### Standalone vs Composed

Each library is self-contained and can be built independently:

```bash
# Standalone: build just GMP
cd gmp && putup configure && putup
```

When composed under the root project, the root `Tuprules.tup` sets toolchain variables
that override each library's `?=` defaults. The same `tup.config` file serves as the
root config in standalone mode and as a scoped subdir config in composed mode.

### Multi-Variant Builds

Build multiple platforms in parallel with a single putup invocation:

```bash
# Configure each variant (different toolchain per variant)
putup configure --config configs/x86_64-linux.config -C . -S ../gcc-15.2.0 -B ../build-gcc-x86_64-linux
putup configure --config configs/aarch64-linux.config -C . -S ../gcc-15.2.0 -B ../build-gcc-aarch64-linux

# Build both variants in parallel
putup -C . -S ../gcc-15.2.0 -B ../build-gcc-x86_64-linux -B ../build-gcc-aarch64-linux -j$(nproc)
```

Or via the Makefile:

```bash
make -f Makefile.pup multi SRCDIR=../gcc-15.2.0
```

Per-library tup.config files are shared across variants — only the root toolchain
config differs. Each `-B` directory gets its own copy of the subdir configs via
`putup configure`.

### Adding a New Platform

Create a new toolchain config file:

```bash
cp configs/x86_64-linux.config configs/myplatform.config
# Edit toolchain vars (CC, AR, HOSTCC)
make -f Makefile.pup PLATFORM=myplatform
```

Per-library configs are platform-independent for this generic C build. For platforms
needing different feature detection, override individual library configs.

## Build Features Demonstrated

- **Assembly support**: CPU-specific `.asm → m4 → assembler` pipeline with config-driven source selection
- **foo-y conditional compilation**: Kbuild-inspired pattern for toggling multi-function sources without `ifeq`
- **Multi-variant parallel builds**: Multiple `-B` directories built simultaneously with different toolchain configs
- **Per-component scoped configs**: Each library has its own `tup.config`, merged with root via scoped config merging
- **Prefix-free config entries**: No `CONFIG_GMP_` prefix needed; directory scope provides the namespace
- **Multi-directory builds**: GMP spans 8 subdirectories, each with its own Tupfile
- **Host tool generation**: GMP table generators compiled and run during the build
- **3-tree builds**: Tupfiles, GCC sources, and build output in separate directories (`-C`, `-S`, `-B`)
- **Cross-directory groups**: Subdirectory objects collected via `../<objs>` into parent
- **Header generation**: gmp.h generated from template via sed with `@()` substitutions
- **Inter-library dependencies**: Order-only groups ensure correct build ordering
- **Self-contained libraries**: Each library has its own `Tuprules.tup` with `?=` defaults; buildable alone or composed
- **Nested `include_rules`**: Root `Tuprules.tup` sets toolchain, per-library `Tuprules.tup` adds flags and bang macros

## Files

| File | Purpose |
|------|---------|
| `Makefile.pup` | Make wrapper for putup commands (`PLATFORM`, `MPN_CPU`) |
| `scripts/resolve-mpn.sh` | Resolve mpn sources for a CPU target |
| `Tupfile.ini` | Project root marker |
| `Tuprules.tup` | Toolchain vars (CC, AR, HOSTCC), library directory names |
| `configs/x86_64-linux.config` | Toolchain config (x86-64 Linux, native) |
| `configs/aarch64-linux.config` | Toolchain config (AArch64 Linux, cross) |
| `gmp/tup.config` | GMP platform config + assembly flags (`GMP_MPARAM`, `ASM_ENABLED`) |
| `gmp/mpn/tup.config` | MPN source resolution (generated by `scripts/resolve-mpn.sh`) |
| `gmp/Tuprules.tup` | GMP CFLAGS, `!cc`, `!m4asm`, `!gen-config` |
| `mpfr/tup.config` | MPFR platform config (prefix-free) |
| `mpfr/Tuprules.tup` | MPFR CFLAGS, `!cc`, `!gen-config` |
| `mpc/tup.config` | MPC platform config (prefix-free) |
| `mpc/Tuprules.tup` | MPC CFLAGS, `!cc`, `!gen-config` |
| `gmp/Tupfile` | GMP config generation, table generators, top-level sources, archive |
| `gmp/{mpn,mpz,mpq,mpf,printf,scanf,rand}/Tupfile` | Per-directory compilation |
| `mpfr/Tupfile` | MPFR config generation |
| `mpfr/src/Tupfile` | MPFR source compilation + archive |
| `mpc/Tupfile` | MPC config generation |
| `mpc/src/Tupfile` | MPC source compilation + archive |

## Assembly Support

By default, GMP builds in generic C mode (equivalent to `--disable-assembly`).
For CPU-specific assembly, pass `MPN_CPU`:

```bash
make -f Makefile.pup MPN_CPU=x86_64           # x86-64 arch-level assembly
make -f Makefile.pup MPN_CPU=x86_64/core2     # CPU-specific assembly
make -f Makefile.pup MPN_CPU=generic           # Pure C (default)
```

### How it works

GMP ships per-CPU assembly routines under `mpn/` (e.g., `mpn/x86_64/`, `mpn/x86_64/core2/`).
The `.asm` files are m4 macros processed into assembly:

```
.asm → m4 (config.m4 + asm-defs.m4) → pipe → assembler → .o
```

The `resolve-mpn` target runs `scripts/resolve-mpn.sh` to walk GMP's priority chain
(`x86_64/core2 → x86_64 → generic`), picking the best available implementation for
each mpn function. Functions without assembly automatically fall back to generic C.

This produces two config files:

| File | Scope | Contents |
|------|-------|----------|
| `gmp/mpn/tup.config` | mpn/ subdirectory | Source lists, per-function y/n toggles |
| `gmp/tup.config` | gmp/ directory | `CONFIG_GMP_MPARAM`, `CONFIG_ASM_ENABLED` |

### The foo-y pattern

Multi-function sources (one `.asm` or `.c` producing multiple `.o` via `-DOPERATION_`)
use the Kbuild-inspired `foo-y` conditional compilation pattern:

```tup
# Variable is non-empty when CONFIG_ASM_add_n=y, empty when unset
aors_add_n-@(ASM_add_n) = x86_64/aors_n.asm

# foreach over a possibly-empty variable: 0 or 1 rules, no conditionals
: foreach $(aors_add_n-y) |> ... -DOPERATION_add_n ... |> add_n.o
```

This keeps `gmp/mpn/Tupfile` completely conditional-free — all source selection is
driven by tup.config entries, resolved once at configure time.

## Notes

- Requires: gcc (or compatible C compiler), m4 (for assembly mode)
- MPFR's `mpfr-mini-gmp.c` compiles to empty when not using mini-gmp mode
