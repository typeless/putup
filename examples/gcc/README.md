# GCC cc1 — C Compiler from Source

Build GCC's `cc1` C compiler from the GCC 15.2.0 source tree using putup.
This builds everything from source: prerequisite math libraries, compiler support
libraries, ~25 host generator programs, and the ~500-file cc1 backend.

## Quick Start

```bash
# 1. Download and extract GCC (includes GMP, MPFR, MPC)
cd /path/to
wget https://gcc.gnu.org/pub/gcc/releases/gcc-15.2.0/gcc-15.2.0.tar.xz
tar xf gcc-15.2.0.tar.xz
cd gcc-15.2.0 && ./contrib/download_prerequisites && cd ..

# 2. Generate pre-generated files (gengtype/genmatch outputs)
cd gcc-15.2.0 && mkdir build && cd build
../configure --disable-bootstrap --enable-languages=c && make -j$(nproc)
cd ../..

# 3. Copy pre-generated files into the example
cd examples/gcc
cp /path/to/gcc-15.2.0/build/gcc/gtype-desc.cc gcc/pre-generated/
cp /path/to/gcc-15.2.0/build/gcc/gt-*.h gcc/pre-generated/
cp /path/to/gcc-15.2.0/build/gcc/gtype-desc.h gcc/pre-generated/
cp /path/to/gcc-15.2.0/build/gcc/gimple-match-*.cc gcc/pre-generated/
cp /path/to/gcc-15.2.0/build/gcc/generic-match-*.cc gcc/pre-generated/

# 4. Build cc1
make -f Makefile.pup SRCDIR=/path/to/gcc-15.2.0 BUILD=../build-gcc
```

## What Gets Built

```
Phase 1-4 (independent libraries, parallel):
  gmp/           → libgmp.a          Math library
  mpfr/          → libmpfr.a         Multi-precision floats (needs GMP)
  mpc/           → libmpc.a          Complex arithmetic (needs GMP + MPFR)
  libiberty/     → libiberty.a       Utility library
  libdecnumber/  → libdecnumber.a    Decimal floating-point (DPD variant)
  libbacktrace/  → libbacktrace.a    Stack unwinding (ELF/mmap)
  libcpp/        → libcpp.a          C preprocessor

Phase 5 (generators, need libiberty):
  gcc/           → genmodes, genattr, genemit, genrecog, ... (~25 host programs)
                 → insn-*.h, insn-*.cc (generated machine descriptions)

Phase 6 (cc1, needs everything):
  gcc/           → cc1              C compiler (~500 objects)
```

## Dependency Chain

```
GMP ──────────────────────────────────→ libgmp.a ──→ cc1
  ↓                                                    ↑
MPFR (needs GMP) ─────────────────→ libmpfr.a ────────┘
  ↓                                                    ↑
MPC (needs GMP + MPFR) ──────────→ libmpc.a ──────────┘
                                                       ↑
libiberty ─→ libiberty.a ─→ generators ─→ insn-*.h ───┘
                               ↓            ↑          ↑
libdecnumber ──────────────→ libdecnumber.a ┘          │
libbacktrace ──────────────→ libbacktrace.a ───────────┘
                                                       ↑
libcpp ────────────────────→ libcpp.a ─────────────────┘
```

## Pre-generated Files

Two GCC generators are too complex to implement initially:

- **gengtype** scans all source files to produce GC type descriptors
  (`gtype-desc.cc`, `gtype-desc.h`, `gt-*.h`)
- **genmatch** depends on libcpp and produces pattern matchers
  (`gimple-match-*.cc`, `generic-match-*.cc`)

These must be copied from a configured GCC build into `gcc/pre-generated/`.
The outputs are target-independent (gengtype) or stable across x86_64 builds
(genmatch), so they only need to be generated once.

## Per-Component Configuration

Each library has its own `tup.config` with prefix-free entries. The directory scope
provides the namespace via scoped config merging:

```
configs/x86_64-linux.config   → Root: CC, CXX, AR, HOSTCC (toolchain)
gmp/tup.config                → GMP: HAVE_ALLOCA, SIZEOF_UNSIGNED_LONG, ...
mpfr/tup.config               → MPFR: HAVE_ALLOCA, HAVE_VA_COPY, ...
mpc/tup.config                → MPC: HAVE_INTTYPES_H, HAVE_LOCALECONV, ...
libiberty/tup.config           → libiberty: HAVE_SBRK, HAVE_MMAP, ...
libdecnumber/tup.config        → libdecnumber: endianness, float format
libbacktrace/tup.config        → libbacktrace: BACKTRACE_ELF_SIZE, HAVE_DL_ITERATE_PHDR, ...
libcpp/tup.config              → libcpp: HAVE_ICONV, ENABLE_NLS, ...
gcc/tup.config                 → GCC: HOST_BITS_PER_*, SIZEOF_*, HAVE_*, ...
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

## Generator Bootstrap

GCC generates machine-specific code from `.md` (machine description) files.
The generators form a bootstrap chain:

```
genmodes (no RTL deps)
  → insn-modes.h, insn-modes-inline.h, min-insn-modes.cc
  → compile min-insn-modes.o

BUILD_RTL objects (need min-insn-modes.o)
  → rtl.o, read-md.o, read-rtl.o, gensupport.o, ...

genconditions → gencondmd.cc → compile+link gencondmd → insn-conditions.md

All other generators (read .md files, produce insn-*.h / insn-*.cc)
  → genattr, genattrtab, genautomata, gencodes, genconfig, ...
  → genemit (10 split outputs), genrecog (10 split outputs + header)
```

## Assembly Support

By default, GMP builds in generic C mode (equivalent to `--disable-assembly`).
For CPU-specific assembly, pass `MPN_CPU`:

```bash
make -f Makefile.pup MPN_CPU=x86_64           # x86-64 arch-level assembly
make -f Makefile.pup MPN_CPU=x86_64/core2     # CPU-specific assembly
make -f Makefile.pup MPN_CPU=generic           # Pure C (default)
```

## Files

| File | Purpose |
|------|---------|
| `Makefile.pup` | Make wrapper for putup commands |
| `scripts/resolve-mpn.sh` | Resolve mpn sources for a CPU target |
| `Tupfile.ini` | Project root marker |
| `Tuprules.tup` | Toolchain vars (CC, CXX, AR, HOSTCC), directory names |
| `configs/x86_64-linux.config` | Toolchain config (x86-64 Linux, native) |
| `configs/aarch64-linux.config` | Toolchain config (AArch64 Linux, cross) |
| **GMP** | |
| `gmp/{Tuprules.tup,tup.config,Tupfile}` | Config, rules, top-level sources |
| `gmp/{mpn,mpz,mpq,mpf,printf,scanf,rand}/Tupfile` | Per-directory compilation |
| **MPFR** | |
| `mpfr/{Tuprules.tup,tup.config,Tupfile}` | Config generation |
| `mpfr/src/Tupfile` | Source compilation + archive |
| **MPC** | |
| `mpc/{Tuprules.tup,tup.config,Tupfile}` | Config generation |
| `mpc/src/Tupfile` | Source compilation + archive |
| **libiberty** | |
| `libiberty/{Tuprules.tup,tup.config,Tupfile}` | Utility library |
| **libdecnumber** | |
| `libdecnumber/{Tuprules.tup,tup.config,Tupfile}` | Decimal number library |
| **libbacktrace** | |
| `libbacktrace/{Tuprules.tup,tup.config,Tupfile}` | Backtrace library |
| **libcpp** | |
| `libcpp/{Tuprules.tup,tup.config,Tupfile}` | C preprocessor library |
| **GCC (cc1)** | |
| `gcc/{Tuprules.tup,tup.config,Tupfile}` | Config headers, generators, backend |
| `gcc/c/Tupfile` | C frontend objects |
| `gcc/c-family/Tupfile` | C-family shared objects |
| `gcc/analyzer/Tupfile` | Static analyzer objects |
| `gcc/config/i386/Tupfile` | x86_64 target-specific objects |
| `gcc/pre-generated/` | gengtype + genmatch outputs |

## Build Features Demonstrated

- **Host-tool bootstrapping**: ~25 generator programs compiled and run during the build
- **Generated source pipeline**: `.md` → generators → `insn-*.h` / `insn-*.cc` → cc1
- **Multi-output generators**: genemit produces 10 split files, genrecog produces 10 + header
- **~500-file C++ compilation**: Full GCC backend with analyzer, C frontend, target-specific code
- **Cross-directory groups**: Subdirectory objects collected via `../<cc1-objs>` into parent
- **Assembly support**: CPU-specific `.asm → m4 → assembler` pipeline with config-driven source selection
- **foo-y conditional compilation**: Kbuild-inspired pattern for toggling multi-function sources without `ifeq`
- **Multi-variant parallel builds**: Multiple `-B` directories built simultaneously with different toolchain configs
- **Per-component scoped configs**: Each library has its own `tup.config`, merged with root via scoped config merging
- **Multi-directory builds**: 15+ subdirectories, each with its own Tupfile
- **3-tree builds**: Tupfiles, GCC sources, and build output in separate directories (`-C`, `-S`, `-B`)
- **Self-contained libraries**: Each library has its own `Tuprules.tup` with `?=` defaults; buildable alone or composed
- **Nested `include_rules`**: Root `Tuprules.tup` sets toolchain, per-library `Tuprules.tup` adds flags and bang macros
- **Inter-library dependencies**: Order-only groups ensure correct build ordering

## Notes

- Requires: gcc, g++, m4 (for GMP assembly mode), gawk (for GCC options pipeline)
- Target: x86_64-linux native (host == target)
- Pre-generated files must be provided from a configured GCC build (see Quick Start)
