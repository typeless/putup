---
name: tupfile-patterns
description: Patterns and gotchas for writing Tupfiles with putup. Use when authoring Tupfiles, debugging build failures, working with 3-tree builds (-C/-S/-B), cross-directory dependencies, order-only groups, bang macros, generator programs, or multi-library projects. Covers Tup syntax that putup supports.
---

# Tupfile Patterns

Reference for writing Tupfiles with putup. See [docs/reference.md](../../../docs/reference.md) for the full manual.

## Rule Syntax

```tup
: [foreach] [inputs] [| order-only] |> command |> [outputs] [| extra-outputs] [{group}] [<order-group>]
```

- `%f` — all inputs, `%o` — all outputs, `%b` — input basename, `%B` — basename without extension
- `%d` — input directory, `%e` — extension, `%%` — literal `%`
- Display text: `^ TEXT ^` right after `|>` on the **same line** (never on a continuation line)

## Variable Expansion vs Shell Variables

putup expands `$(VAR)` as a Tup variable. Bare `$VAR` passes through to the shell unchanged.

```tup
# $(CC) expanded by putup at parse time
# $PWD passed through to /bin/sh at execution time
: |> $(CC) -DPREFIX=$PWD -c %f -o %o |> output.o
```

This is useful for shell tricks like capturing CWD before `cd`:

```tup
: |> SRCDIR=$PWD && cd $(TUP_VARIANT_OUTPUTDIR) && ./tool $SRCDIR/input |> output
```

## Display Text Gotcha

Display text `^ ... ^` **must** appear on the same line as the opening `|>`. If placed on a continuation line, it becomes part of the shell command.

```tup
# CORRECT
: input |> ^ CC %b^ $(CC) -c %f -o %o |> output.o

# CORRECT (continuation after display text)
: input |> ^ CC %b^ \
  $(CC) -c %f -o %o |> output.o

# WRONG — ^ becomes a shell command
: input |> \
  ^ CC %b^ $(CC) -c %f -o %o |> output.o
```

## 3-Tree Builds (-C / -S / -B)

```bash
putup -C config_dir -S source_dir -B build_dir
```

- `-C` — config tree (Tupfiles, tup.config, Tuprules.tup)
- `-S` — source tree (read-only source code)
- `-B` — build tree (output directory)

Commands run with CWD = source directory. The source tree is **read-only**.

### Key Variables in 3-Tree Mode

| Variable | Meaning |
|----------|---------|
| `$(TUP_CWD)` | Config-relative directory of current Tupfile |
| `$(TUP_VARIANT_OUTPUTDIR)` | Absolute path to the build directory |
| `$(S)` (convention) | Usually set to `$(TUP_CWD)` in root Tuprules.tup |
| `$(B)` (convention) | Usually set to `$(TUP_VARIANT_OUTPUTDIR)/$(S)` |

### Generator Programs in 3-Tree Mode

Generators that write output files via flags (not stdout) write relative to CWD, which is the read-only source dir. Two approaches:

**Approach 1: cd to build dir** (preferred for programs that write many files by name)

```tup
: $(MD) | generator |> ^ GEN outputs^ \
  SRCDIR=$PWD && cd $(TUP_VARIANT_OUTPUTDIR) && \
  ./generator $SRCDIR/input1.md $SRCDIR/input2.md \
  -O output-1.cc -O output-2.cc \
  |> output-1.cc output-2.cc
```

Note: inputs that are already in the build dir (previously generated) don't need `$SRCDIR/`.

**Approach 2: stdout redirect** (preferred for single-output generators)

```tup
: input.md | generator |> ^ GEN %o^ $(TUP_VARIANT_OUTPUTDIR)/generator %f > %o |> output.h
```

## Order-Only Groups

Groups create ordering dependencies without file-level dependencies.

```tup
# Producer: outputs go into <gen-headers> group
: |> gen-header > %o |> config.h <gen-headers>
: |> gen-header > %o |> types.h <gen-headers>

# Consumer: waits for all <gen-headers> members before starting
: src.c | <gen-headers> |> $(CC) -c %f -o %o |> src.o
```

Cross-directory groups use the `$(S)/dir/` prefix:

```tup
# In lib/Tupfile: produce into group
: |> generate > %o |> header.h <gen-headers>

# In app/Tupfile: consume from lib's group
: src.c | $(S)/lib/<gen-headers> |> $(CC) -c %f -o %o |> src.o
```

## Bang Macros

Define reusable command templates in `Tuprules.tup`:

```tup
!cc = |> ^ CC %b^ $(CC) $(CFLAGS) -c %f -o %o |>

# With order-only deps (consumers auto-wait)
!cc = | <gen-headers> |> ^ CC %b^ $(CC) $(CFLAGS) -c %f -o %o |>
```

Usage in Tupfile:

```tup
: foreach *.c |> !cc |> %B.o
```

## Self-Contained Library Convention

For multi-library projects (like GCC with GMP, MPFR, MPC):

**Root `Tuprules.tup`** — sets layout variables:

```tup
S = $(TUP_CWD)
B = $(TUP_VARIANT_OUTPUTDIR)/$(S)
GMP_DIR = gcc/gmp
MPFR_DIR = gcc/mpfr
```

**Each library's `Tuprules.tup`** — uses `?=` defaults for standalone builds:

```tup
S ?= $(TUP_CWD)
B ?= $(TUP_VARIANT_OUTPUTDIR)/$(S)
GMP_DIR ?= .
CFLAGS = -I$(B)/$(GMP_DIR) -I$(S)/$(GMP_DIR)
!cc = |> ^ CC %b^ $(CC) $(CFLAGS) -c %f -o %o |>
```

`?=` means "set only if undefined". When built as part of the larger project, the root Tuprules.tup values win. When built standalone, the `?=` defaults apply.

Prefixed DIR vars (`GMP_DIR`, `MPFR_DIR`) are needed because `include_rules` merges ALL `Tuprules.tup` from root to leaf — a single `DIR` variable would collide.

## Scoped tup.config

Each subdirectory can have its own `tup.config` for `@(VAR)` config variables:

```
project/
  tup.config           # Global: CC=gcc, AR=ar
  gmp/tup.config       # GMP-specific: HAVE_ALLOCA=1
  mpfr/tup.config      # MPFR-specific: HAVE_LOCALE=1
```

Run `putup configure` to propagate config files into the build directory. Parent config values override child values (parent wins on conflict).

## `!gen-config` Pattern

Convert `tup.config` variables into C `#define` headers:

```tup
!gen-config = |> ^ GEN %o^ awk -F= \
  '/^CONFIG_/{k=substr($1,8);v=substr($0,length($1)+2); \
  if(v!="n")print "#define " k " " (v=="y"?1:v)}' \
  $(B)/$(GCC_DIR)/tup.config > %o |>
```

`CONFIG_HAVE_MMAP=1` becomes `#define HAVE_MMAP 1`. `CONFIG_HAVE_UCHAR=n` is suppressed.

## Line Continuation

Use `\` at end of line. Works in rules and variable assignments:

```tup
SRCS  = file1.c
SRCS += file2.c \
        file3.c \
        file4.c
```

## Conditional Compilation

```tup
ifdef CC
  CFLAGS += -Wall
endif

ifeq ($(TARGET),arm)
  CFLAGS += -marm
endif
```
