# GCC Cross-Compiler Toolchain from Source

Build a complete GCC 15.2.0 cross-compiler toolchain using putup.
This builds everything from source: prerequisite math libraries, compiler support
libraries, ~25 host generator programs, the ~500-file compiler backend, C/C++
frontends, driver programs, and developer tools — 3500+ build commands in ~130s.

## Quick Start

```bash
# 1. Download and extract GCC (includes GMP, MPFR, MPC)
cd /path/to
wget https://gcc.gnu.org/pub/gcc/releases/gcc-15.2.0/gcc-15.2.0.tar.xz
tar xf gcc-15.2.0.tar.xz
cd gcc-15.2.0 && ./contrib/download_prerequisites && cd ..

# 2. Build cc1
cd examples/gcc
make -f Makefile.pup SRCDIR=/path/to/gcc-15.2.0 BUILD=../build-gcc
```

## What Gets Built

```
Libraries (parallel):
  gmp/           → libgmp.a          Math library
  mpfr/          → libmpfr.a         Multi-precision floats (needs GMP)
  mpc/           → libmpc.a          Complex arithmetic (needs GMP + MPFR)
  libiberty/     → libiberty.a       Utility library
  libdecnumber/  → libdecnumber.a    Decimal floating-point (DPD variant)
  libbacktrace/  → libbacktrace.a    Stack unwinding (ELF/mmap)
  libcpp/        → libcpp.a          C preprocessor
  libcody/       → libcody.a         C++ modules protocol

Generators (need libiberty):
  gcc/           → genmodes, genattr, genemit, genrecog, ... (~25 host programs)
                 → insn-*.h, insn-*.cc (generated machine descriptions)
                 → gtype-desc.cc, gt-*.h (~60 GC type descriptor headers)

Compilers:
  gcc/           → cc1              C compiler backend (~500 objects)
  gcc/           → cc1plus          C++ compiler backend (+ 40 cp/ objects)

Drivers:
  gcc/           → xgcc             C compiler driver
  gcc/           → xg++             C++ compiler driver
  gcc/           → cpp              C preprocessor driver

Tools:
  gcc/           → collect2         Linker wrapper (C++ ctor/dtor collection)
  gcc/           → lto-wrapper      LTO linker plugin interface
  gcc/           → gcov             Code coverage analysis
  gcc/           → gcov-dump        Coverage data inspector
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

### Multi-Platform Support

The build uses config-variable substitution for platform selection. Target-specific
rules (tm.h, tm_p.h, MD files, gtyp entries) live in `gcc/targets/@(TARGET).tup`:

```
configs/x86_64-linux.config         → Native Linux x86-64
configs/darwin-x86_64-linux.config  → macOS host, Linux x86-64 target
configs/host-darwin/                → macOS-specific library configs
gcc/targets/x86_64-pc-linux-gnu.tup → x86_64-linux target rules
```

**Cross-compiler (macOS host → x86-64 Linux target):**

```bash
make -f Makefile.pup PLATFORM=darwin-x86_64-linux HOST=darwin
```

This builds cc1 using Apple Clang on macOS, producing a compiler that generates
x86_64 Linux code. The `HOST=darwin` flag copies macOS-specific library configs
(feature detection for macOS: no `-ldl`, Mach-O libbacktrace, different iconv, etc.)

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

gengtype (GC type descriptors, needs libiberty)
  → parses GTY annotations from gtyp-input.list
  → gtype-desc.cc, gtype-desc.h, gtype-c.h, gt-*.h (~60 headers)
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
| **libcody** | |
| `libcody/{Tuprules.tup,tup.config,Tupfile}` | C++ modules protocol library |
| **GCC (compilers + tools)** | |
| `gcc/{Tuprules.tup,tup.config,Tupfile}` | Config headers, generators, backend, tools |
| `gcc/c/Tupfile` | C frontend objects |
| `gcc/cp/Tupfile` | C++ frontend objects |
| `gcc/c-family/Tupfile` | C-family shared objects |
| `gcc/analyzer/Tupfile` | Static analyzer objects |
| `gcc/config/i386/Tupfile` | x86_64 target-specific objects |
| `gcc/targets/x86_64-pc-linux-gnu.tup` | Target-specific machine description rules |

## Build Features Demonstrated

- **Host-tool bootstrapping**: ~25 generator programs compiled and run during the build
- **Generated source pipeline**: `.md` → generators → `insn-*.h` / `insn-*.cc` → cc1
- **GC type descriptor generation**: gengtype parses GTY annotations across all source files, producing `gtype-desc.cc` and ~60 `gt-*.h` headers
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
- All generators (including gengtype) are built and run as part of the normal build — no `./configure && make` step required
