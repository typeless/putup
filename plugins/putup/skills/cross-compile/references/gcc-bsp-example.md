# GCC BSP Example

Reference for the `examples/bsp/gcc/` directory, which builds GCC and binutils from source using putup in 3-tree mode.

## Build Command

```bash
cd /path/to/pup/examples/bsp && \
  scripts/download-source.sh ../../source-root && \
  putup configure --config configs/x86_64-linux.config \
    -C . -S ../../source-root -B ../../build-gcc && \
  putup -C . -S ../../source-root -B ../../build-gcc -j$(nproc)
```

## Build Phases

GCC builds in 14 ordered phases inside `gcc/gcc/Tupfile`:

| Phase | What                     | Key output                          |
|-------|--------------------------|-------------------------------------|
| 1     | Config headers           | auto-host.h, tm.h, options.h       |
| 2     | Generator bootstrap      | genmodes, insn-modes.h, BUILD_RTL  |
| 3     | RTL generators           | 20+ insn-*.h/cc files              |
| 4     | Generated source compile | Generated .cc into `<cc1-objs>`    |
| 5     | Backend objects          | ~445 gcc/ source files              |
| 6     | Common objects           | libcommon + libcommon-target        |
| 7     | cc1 link                 | C compiler                          |
| 8     | xgcc driver              | gcc/g++ driver                      |
| 9     | cc1plus                  | C++ compiler (gcc/cp/ frontend)     |
| 10    | collect2                 | Linker wrapper                      |
| 11    | xg++ driver              | C++ compilation driver              |
| 12    | cpp                      | Standalone C preprocessor           |
| 13    | lto-wrapper              | LTO linker plugin                   |
| 14    | gcov + gcov-dump         | Code coverage tools                 |

## Group Dependencies

| Group                  | Purpose               | Producers               |
|------------------------|-----------------------|-------------------------|
| `<gen-config-headers>` | Config/target headers | Phase 1                 |
| `<gen-insn-headers>`   | Generated insn headers| Phase 3 generators      |
| `<gen-gtype-headers>`  | GC type descriptors   | gengtype                |
| `<cc1-objs>`           | All object files      | Phases 4-6              |

## HOST vs TARGET Compilation

Generators run on the build machine (HOST). Compiled objects target the output platform (TARGET).

```tup
HOST_CXXFLAGS = -O0 -std=gnu++14 -DIN_GCC -DGENERATOR_FILE -DHAVE_CONFIG_H
TARGET_CXXFLAGS = -O2 -std=gnu++14 -DIN_GCC -DHAVE_CONFIG_H
```

`-DGENERATOR_FILE` is the key difference. It gates which headers generators see -- they skip insn-flags.h and insn-modes.h via guards in tm.h.

## Special Generator Patterns

### Stdout generators (simple redirect)

```tup
: $(MD) insn-conditions.md | gen-foo |> ^ GEN %o^ \
  $(TUP_VARIANT_OUTPUTDIR)/gen-foo %f > %o |> insn-foo.h
```

### File-output generators (cd to build dir)

Generators with `-O`, `-H`, `-A`, `-D`, `-L`, `-h`, `-c` flags write files relative to CWD:

```tup
: $(MD) insn-conditions.md | generator |> ^ GEN outputs^ \
  cd $(TUP_VARIANT_OUTPUTDIR) && ./generator %f \
  -O output-1.cc -O output-2.cc \
  |> output-1.cc output-2.cc
```

When input paths break after cd, capture CWD:

```tup
: $(MD) insn-conditions.md | generator |> ^ GEN outputs^ \
  SRCDIR=$PWD && cd $(TUP_VARIANT_OUTPUTDIR) && \
  ./generator $SRCDIR/common.md $SRCDIR/config/i386/i386.md \
  -O output-1.cc -O output-2.cc \
  |> output-1.cc output-2.cc
```

### genmatch (special case)

genmatch uses `getpwd()` for include search. Must cd to build dir AND pass source-relative match.pd:

```tup
: $(SRC)/match.pd | genmatch cfn-operators.pd |> ^ GEN gimple-match-*^ \
  SRCDIR=$PWD && cd $(TUP_VARIANT_OUTPUTDIR) && ./genmatch --gimple \
  --header=gimple-match-auto.h --include=gimple-match-auto.h \
  $SRCDIR/match.pd \
  gimple-match-1.cc ... gimple-match-10.cc \
  |> gimple-match-auto.h gimple-match-1.cc ... gimple-match-10.cc
```

### gengtype (two-pass, different flag)

gengtype uses `-DHOST_GENERATOR_FILE` (NOT `-DGENERATOR_FILE`) because it needs full host header access:

```tup
# Pass 1: write state file
gengtype -S $(SRC) -I gtyp-input.list -w gtype.state
# Pass 2: generate from state
cd $(TUP_VARIANT_OUTPUTDIR) && ./gengtype -r gtype.state
```

## Config Header Gotchas

### auto-host.h

Generated from tup.config via `!gen-config`. Critical rule: do NOT set `CONFIG_HAVE_UCHAR=1`. Modern Linux does not provide `uchar`. GCC's coretypes.h typedefs it when `HAVE_UCHAR` is NOT defined. Setting it causes a compile error.

### tm.h (target machine)

Must match real configure output. Structure:

1. LIBC defines outside `#ifdef IN_GCC`
2. Target headers inside `#ifdef IN_GCC`
3. `insn-flags.h` guarded with `!defined GENERATOR_FILE && !defined USED_FOR_TARGET`
4. `insn-modes.h` guarded with `!defined GENERATOR_FILE`
5. `defaults.h` at the end

### tm_p.h (target machine prototypes)

Must include exactly three headers:

```c
#include "config/i386/i386-protos.h"
#include "config/linux-protos.h"
#include "tm-preds.h"
```

`tm-preds.h` is generated by `genpreds -h`. Missing it causes "undeclared" errors in many files for functions like `register_operand`.

### bconfig.h vs config.h

- `bconfig.h` -- for generators: includes `auto-host.h` + `ansidecl.h`
- `config.h` -- for target objects: includes `auto-host.h`, errors if `GENERATOR_FILE` defined

## Self-Contained Libraries

Each library (gmp, mpfr, mpc, libiberty, libcpp, libdecnumber, libbacktrace) has:

- Its own `Tuprules.tup` with `?=` defaults for standalone builds
- Its own `defaults.config` for library-specific config
- Its own `Tupfile` for compilation rules

The root `Tuprules.tup` sets prefixed DIR vars (`GMP_DIR = gmp`, `MPFR_DIR = mpfr`) that override the `?=` defaults when building as part of the larger project.

## Debugging Workflow

1. Run the build command
2. Read the **first** error (ignore cascading failures)
3. Diagnose the category:
   - Missing header -- check group dependencies
   - Undeclared symbol -- check which generator produces the header
   - Read-only filesystem -- generator needs cd to build dir
   - Wrong define -- check tup.config and `!gen-config` output
4. Fix the Tupfile or tup.config
5. If tup.config changed, re-run `putup configure`
6. Rebuild and repeat
