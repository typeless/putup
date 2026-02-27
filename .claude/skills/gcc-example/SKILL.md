---
name: gcc-example
description: Building GCC with putup. Use when working on the examples/bsp/ BSP directory, debugging GCC build failures, adding new GCC source files to the build, fixing generator issues, or extending the toolchain. Covers 3-tree mode workflow, generator patterns, config headers, and accumulated fixes.
---

# GCC Example Build

The `examples/bsp/` BSP directory builds GCC and binutils from source using putup in 3-tree mode. GCC-specific Tupfiles live under `examples/bsp/gcc/`. This skill captures the architecture and hard-won knowledge from iterative debugging.

## Build Command

```bash
cd /path/to/pup/examples/bsp && \
  make -f Makefile.pup SRCDIR=../../source-root
```

Or directly with putup:

```bash
cd /path/to/pup/examples/bsp && \
  putup configure --config configs/x86_64-linux.config \
    -C . -S ../../source-root -B ../../build-gcc
  putup -C . -S ../../source-root -B ../../build-gcc -j$(nproc)
```

- `-C .` — config tree (BSP root: Tupfiles, tup.config, Tuprules.tup)
- `-S ../source-root` — assembled source tree (gcc/ + binutils/ subdirs, read-only)
- `-B ../build-gcc` — build output directory

After editing any `tup.config`, re-run `putup configure` to propagate to the build dir.

## Architecture

14 build phases in `gcc/gcc/Tupfile`:

1. **Config headers** (`<gen-config-headers>`) — auto-host.h, bconfig.h, config.h, tm.h, tm_p.h, options.h, etc.
2. **Generator bootstrap** — genmodes → insn-modes.h → BUILD_RTL → genconditions → insn-conditions.md
3. **RTL generators** — 20+ generators producing insn-*.h/cc files (`<gen-insn-headers>`)
4. **Generated source compilation** — compile all generated .cc into `<cc1-objs>`
5. **Backend objects** — ~445 source files from gcc/
6. **Common objects** — OBJS-libcommon + OBJS-libcommon-target
7. **cc1 link** — link everything with library archives
8. **xgcc driver** — main gcc/g++ driver executable
9. **cc1plus** — C++ compiler (C++ frontend objects from gcc/cp/)
10. **collect2** — linker wrapper
11. **g++ driver** (xg++) — C++ compilation driver
12. **cpp** — standalone C preprocessor
13. **lto-wrapper** — LTO linker plugin interface
14. **gcov + gcov-dump** — code coverage tools

### Group Dependencies

| Group | Purpose | Producers |
|-------|---------|-----------|
| `<gen-config-headers>` | Config/target headers | Phase 1 (auto-host.h, tm.h, options.h, etc.) |
| `<gen-insn-headers>` | Generated insn headers | Phase 3 generators |
| `<gen-gtype-headers>` | GC type descriptors | gengtype |
| `<cc1-objs>` | All object files | Phases 4-6 |

### Two Compilation Modes

**HOST** (generators) — runs on build machine:
```
HOST_CXXFLAGS = -O0 -std=gnu++14 -DIN_GCC -DGENERATOR_FILE -DHAVE_CONFIG_H
```

**TARGET** (cc1 objects) — runs on target:
```
TARGET_CXXFLAGS = -O2 -std=gnu++14 -DIN_GCC -DHAVE_CONFIG_H
```

`-DGENERATOR_FILE` is the key difference. It gates which headers generators see (they skip insn-flags.h, insn-modes.h via guards in tm.h).

## Generator Patterns

### Stdout Generators (simple — just redirect)

```tup
: $(MD) insn-conditions.md | gen-foo |> ^ GEN %o^ \
  $(TUP_VARIANT_OUTPUTDIR)/gen-foo %f > %o |> insn-foo.h
```

### File-Output Generators (need cd to build dir)

Generators with `-O`, `-H`, `-A`, `-D`, `-L`, `-h`, `-c` flags write files relative to CWD. In 3-tree mode CWD is the read-only source dir, so cd first:

```tup
: $(MD) insn-conditions.md | generator |> ^ GEN outputs^ \
  cd $(TUP_VARIANT_OUTPUTDIR) && ./generator %f \
  -O output-1.cc -O output-2.cc \
  |> output-1.cc output-2.cc
```

If `%f` produces relative paths that break after cd, capture CWD first:

```tup
: $(MD) insn-conditions.md | generator |> ^ GEN outputs^ \
  SRCDIR=$PWD && cd $(TUP_VARIANT_OUTPUTDIR) && \
  ./generator $SRCDIR/common.md $SRCDIR/config/i386/i386.md insn-conditions.md \
  -O output-1.cc -O output-2.cc \
  |> output-1.cc output-2.cc
```

Note: `$SRCDIR` and `$PWD` are shell variables (bare `$` passes through putup to shell).

### genmatch (special case)

genmatch uses libcpp's `getpwd()` as include search path. `match.pd` includes `cfn-operators.pd` which lives in the build dir. Must cd to build dir AND pass source-relative match.pd path:

```tup
: $(SRC)/match.pd | genmatch cfn-operators.pd |> ^ GEN gimple-match-*^ \
  SRCDIR=$PWD && cd $(TUP_VARIANT_OUTPUTDIR) && ./genmatch --gimple \
  --header=gimple-match-auto.h --include=gimple-match-auto.h \
  $SRCDIR/match.pd \
  gimple-match-1.cc ... gimple-match-10.cc \
  |> gimple-match-auto.h gimple-match-1.cc ... gimple-match-10.cc
```

### gengtype (two-pass)

gengtype scans GTY-annotated source files. Uses `-DHOST_GENERATOR_FILE` (NOT `-DGENERATOR_FILE`) because it needs full host header access:

```tup
# Pass 1: write state file
gengtype -S $(SRC) -I gtyp-input.list -w gtype.state
# Pass 2: generate from state
cd $(TUP_VARIANT_OUTPUTDIR) && ./gengtype -r gtype.state
```

## Config Headers

### auto-host.h (from tup.config via !gen-config)

`CONFIG_FOO=value` in `tup.config` → `#define FOO value` in auto-host.h.

Key gotchas:
- **Do NOT set `CONFIG_HAVE_UCHAR=1`** — modern Linux doesn't provide `uchar`. GCC's coretypes.h typedefs it when `HAVE_UCHAR` is NOT defined.
- Cross-reference with real `./configure` output for your platform.

### tm.h (target machine)

Must match the real configure-generated layout exactly. Key structure:
- LIBC defines (GLIBC, UCLIBC, etc.) outside `#ifdef IN_GCC`
- Target headers inside `#ifdef IN_GCC`
- `insn-flags.h` guarded with `!defined GENERATOR_FILE && !defined USED_FOR_TARGET`
- `insn-modes.h` guarded with `!defined GENERATOR_FILE`
- `defaults.h` at the end

### tm_p.h (target machine prototypes)

Must include exactly three headers:
```c
#include "config/i386/i386-protos.h"
#include "config/linux-protos.h"
#include "tm-preds.h"
```

The `tm-preds.h` is generated by `genpreds -h` and declares predicate functions like `register_operand`. Missing this causes "undeclared" errors in many files.

### bconfig.h vs config.h

- `bconfig.h` — for generators: includes `auto-host.h` + `ansidecl.h`
- `config.h` — for target objects: includes `auto-host.h`, errors if `GENERATOR_FILE` defined

## Self-Contained Library Convention

Each library (gmp, mpfr, mpc, libiberty, libcpp, libdecnumber, libbacktrace) has:
- Its own `Tuprules.tup` with `?=` defaults
- Its own `tup.config` for library-specific config
- Its own `Tupfile` for compilation rules

The root `Tuprules.tup` sets prefixed DIR vars (`GMP_DIR = gmp`, etc.) that override the `?=` defaults when building as part of the larger project.

## Debugging Workflow

1. Run the build command
2. Read the **first** error (ignore cascading failures)
3. Diagnose: missing header? → check group deps. Undeclared symbol? → check which generated header provides it. Read-only filesystem? → generator needs cd to build dir.
4. Fix the Tupfile or tup.config
5. If tup.config changed, run `putup configure` to propagate
6. Rebuild and repeat

## Common Error Patterns

| Error | Likely Cause | Fix |
|-------|-------------|-----|
| `Read-only file system` | Generator writes to CWD (source dir) | cd to build dir before running |
| `No such file or directory` (generator output) | Same as above | cd to build dir |
| `undeclared` symbol from generated header | Missing group dependency or header include | Check which generator produces it |
| `/bin/sh: ^: not found` | Display text on continuation line | Move `^ ... ^` to same line as `|>` |
| `uchar not declared` | `HAVE_UCHAR` wrongly defined | Remove `CONFIG_HAVE_UCHAR` from tup.config |
| `register_operand undeclared` | `tm_p.h` missing `tm-preds.h` | Add `#include "tm-preds.h"` to tm_p.h generation |
