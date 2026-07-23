---
name: tupfile-authoring
description: Writing Tupfiles with putup. Use when authoring Tupfiles, debugging build failures, working with rule syntax, variables, bang macros, groups, conditionals, or generators.
---

# Tupfile Authoring

Guide for writing Tupfiles with putup. For the full manual, see
<https://github.com/typeless/putup/blob/main/docs/reference.md>.

## 1. Rule Syntax

```
: [foreach] [inputs] [| order-only] |> command |> [outputs] [| extra-outputs] [{group}] [<order-group>]
```

```tup
: main.c |> gcc -c %f -o %o |> main.o              # simple
: foreach *.c |> gcc -c %f -o %o |> %B.o            # one command per input
: foo.o bar.o |> gcc %f -o %o |> program             # multiple inputs
: main.c | config.h |> gcc -c %f -o %o |> main.o    # order-only after |
```

## 2. Pattern Flags

| Flag | Expands to | Example result |
|------|-----------|----------------|
| `%f` | All inputs | `foo.c bar.c` |
| `%o` | All outputs | `foo.o` |
| `%b` | Input basename (with ext) | `foo.c` |
| `%B` | Input basename (no ext) | `foo` |
| `%d` | Input directory | `src` |
| `%e` | Extension | `c` |
| `%g` | Glob match portion | `foo` from `*_test.c` + `foo_test.c` |
| `%%` | Literal `%` | `%` |

Numbered variants select a specific input or output:

```tup
: header.h template.c |> gen %1f %2f -o %o |> output.c
```

## 3. Variable Expansion -- the #1 Gotcha

putup expands `$(VAR)` at parse time. Bare `$VAR` passes through to the shell.

```tup
# $(CC) is expanded by putup; $PWD is expanded by /bin/sh
: |> $(CC) -DPREFIX=$PWD -c %f -o %o |> output.o
```

Use this to capture CWD before `cd`:

```tup
: |> SRCDIR=$PWD && cd $(TUP_VARIANT_OUTPUTDIR) && ./tool $SRCDIR/input |> output
```

GNU Make functions are **not** supported: `$(notdir $<)`, `$(wildcard ...)`,
etc. silently expand to empty (in tup too). Watch for them in Tupfiles ported
from Make.

## 4. Assignment Operators

| Operator | Name | Behavior |
|----------|------|----------|
| `=` | Set | Replace value |
| `+=` | Append | Append (space-separated) |
| `:=` | Define | Assign literal string, no expansion |
| `?=` | Soft set | Set only if undefined (first wins) |
| `??=` | Weak set | Deferred default (last wins) |

```tup
CC ?= gcc          # first ?= wins
CC ?= clang        # ignored -- CC already defined

CFLAGS ??= -O0     # fallback if nothing else sets CFLAGS
CFLAGS ??= -O2     # this wins (last ??= wins)

CC = clang         # unconditional set always wins
```

`?=` and `??=` check whether a variable is *defined*, not whether it is empty.

## 5. Config Variables

Config values live in `tup.config` with a `CONFIG_` prefix:

```ini
CONFIG_CC=clang
CONFIG_DEBUG=y
```

Access them in Tupfiles with `@(NAME)` (the `CONFIG_` prefix is stripped):

```tup
CC = @(CC)
DEBUG = @(DEBUG:-n)       # default value if unset
```

The full-prefix form `$(CONFIG_NAME)` also reads tup.config (variables starting
with `CONFIG_` cannot be set in Tupfiles). Kernel-style projects use it to
compose variable names:

```tup
srcs-$(CONFIG_KERNEL_FS) += fs.c      # appends to srcs-y when CONFIG_KERNEL_FS=y
: foreach $(srcs-y) |> !cc |> %B.o {objs}
```

Override from the CLI:

```bash
putup -D CC=clang -D DEBUG=y
```

## 6. Bang Macros

Define reusable command templates in `Tuprules.tup`:

```tup
!cc = |> ^ CC %b^ $(CC) $(CFLAGS) -c %f -o %o |> %B.o
!link = |> ^ LINK %o^ $(CC) $(LDFLAGS) %f -o %o |>
```

Invoke in a Tupfile:

```tup
include_rules
: foreach *.c |> !cc |> {objs}
: {objs} |> !link |> program
```

Embed order-only deps in the macro so every consumer waits automatically:

```tup
!cc = | <gen-headers> |> ^ CC %b^ $(CC) $(CFLAGS) -c %f -o %o |> %B.o
```

Select a macro implementation per config by defining it inside a conditional —
rules after `endif` use whichever branch was active:

```tup
ifeq ($(CONFIG_DEVICE_MH1903),y)
  !to_bin = | tools/hex2bin |> srec_cat %f -intel -o - -binary | tools/hex2bin > %o |>
else
  !to_bin = |> srec_cat %f -intel -o %o -binary |>
endif

: foreach $(mods-y) |> !to_bin |> %B.bin
```

## 7. Groups

**Output group** -- collect outputs for later use as inputs:

```tup
: foreach *.c |> !cc |> %B.o {objs}
: {objs} |> !link |> program
```

**Order-only group** -- establish ordering without data dependency:

```tup
: |> gen-header > %o |> config.h <gen-headers>
: src.c | <gen-headers> |> $(CC) -c %f -o %o |> src.o
```

**Cross-directory groups** -- prefix with the path to the producing directory:

```tup
# In lib/Tupfile
: |> generate > %o |> header.h <gen-headers>

# In app/Tupfile ($(S) is the source root convention)
: src.c | $(S)/lib/<gen-headers> |> $(CC) -c %f -o %o |> src.o
```

**Group expansion in commands** -- `%<name>` expands to the group's member
paths, so a group can be aggregated without listing files:

```tup
# Modules contribute: ... |> %d.header $(ROOT)/modules/<json-headers>
: $(ROOT)/modules/<json-headers> |> cat %<json-headers> > %o |> header
```

**Composing nested projects** -- a subdirectory with its own `Tupfile.ini` is
a separate project and is pruned from this build; a group (or generated-file)
reference under it composes the whole nested project back in, at project
granularity. See the composable-libraries skill for the full pattern.

## 8. Conditionals

```tup
ifdef DEBUG
  CFLAGS += -g -O0
else
  CFLAGS += -O2
endif

ifeq ($(TUP_PLATFORM),linux)
  LDFLAGS += -lpthread
endif

ifeq (@(ENABLE_TESTS),y)
  : foreach test_*.c |> !cc |> {test_objs}
endif
```

`ifdef NAME` / `ifndef NAME` take a bare name and test **definedness** (empty
still counts as defined). Tupfile variables are checked first -- a deliberate
putup extension; tup checks only `@`-variables -- then config variables:
`ifdef FOO` matches `CONFIG_FOO=...` in tup.config (`CONFIG_` prefix
optional), and `putup -D FOO` (shorthand for `FOO=y`) toggles it from the
CLI.

**Conditional source pattern** -- append sources guarded by config:

```tup
srcs += always.c
srcs-@(FEATURE_FOO) += foo.c
srcs-@(FEATURE_BAR) += bar.c

: foreach $(srcs) $(srcs-y) |> !cc |> {objs}
```

Tup deduplicates inputs, so listing a file under multiple config guards is
safe. Kernel-style projects write the guard as `srcs-$(CONFIG_FEATURE_FOO)` --
same effect, see §5.

## 9. Display Text

Custom display text replaces the raw command in build output. Place
`^ TEXT ^` right after the opening `|>`, on the **same line**:

```tup
# CORRECT
: input |> ^ CC %b^ $(CC) -c %f -o %o |> output.o

# CORRECT (continuation after display text is fine)
: input |> ^ CC %b^ \
  $(CC) -c %f -o %o |> output.o

# WRONG -- ^ becomes a shell command
: input |> \
  ^ CC %b^ $(CC) -c %f -o %o |> output.o
```

## 10. Line Continuation

Use `\` at end of line to split long rules. Place `|>` at the start of
continuation lines for readability:

```tup
: main.c utils.c \
|> $(CC) $(CFLAGS) -c %f -o %o \
|> main.o

CFLAGS += -Wall \
          -Wextra \
          -O2
```

The `|>` delimiters become visual column anchors separating inputs, command,
and outputs. This is the recommended style for multi-line rules.

## 11. `import` and `export`

`import` reads an environment variable into Tup's namespace at parse time.
`export` makes a Tup variable available to shell subprocesses.

```tup
import CC=gcc               # read $CC from env, default to gcc
import CROSS_COMPILE=       # read $CROSS_COMPILE, default empty
export PKG_CONFIG_PATH      # pass to shell subprocesses
```

Without `export`, shell `$VAR` is empty even if `$(VAR)` expands correctly.

## 12. Shared Rule Templates

For projects with many similar targets, factor the build rules into a shared
`.tup` file. Each Tupfile becomes a pure source list:

```tup
# lib.tup — shared template (included by each library Tupfile)
: foreach $(srcs-y) | <gen-headers> \
|> !cc \
|> %B.o {objs}
: {objs} \
|> $(AR) rcs %o %f \
|> built-in.o
```

```tup
# modules/kernel/Tupfile — just lists sources
include_rules
srcs-y += main.c
srcs-$(CONFIG_FS) += fs.c
srcs-$(CONFIG_NET) += network.c
include $(ROOT)/lib.tup
```

This separates "what to build" from "how to build." Adding a source file
means adding one line to a Tupfile — no rule changes needed.

## 13. Glob Tradeoffs

`srcs-y += *.c` works but has tradeoffs:

- Adding/removing files is implicit (no Tupfile edit, but no review visibility)
- No way to exclude one file from a glob
- Explicit file lists are more maintainable for larger directories

Use globs for small stable directories. Use explicit lists when files change
frequently or when you need to exclude specific files.

## 14. Debugging Build Failures

**Read the first error.** putup prints errors in dependency order.
Fix the first one -- later errors often cascade from it.

**Parse without building** to check syntax:

```bash
putup parse -v             # verbose: shows each Tupfile as parsed
putup parse                # reports composability-convention violations (warnings)
putup parse --check=error  # fail on those violations -- for CI (alias: --strict)
```

**Inspect variables:**

```bash
putup show var CC        # see where CC was assigned and overridden
putup show var --json    # machine-readable output
```

**Common error patterns:**

| Symptom | Likely cause |
|---------|-------------|
| `command not found` | `$(CC)` undefined or `$CC` passed to shell without export |
| Output written to wrong directory | Missing `cd $(TUP_VARIANT_OUTPUTDIR)` for generators |
| `No tup.config found` | Run `putup configure` first |
| Circular dependency | `<group>` references forming a loop |
| File not found during build | Cross-dir group path wrong; check `$(S)/dir/<group>` |
| Variable has unexpected value | Run `putup show var NAME` to see assignment history |

**Generate a dependency graph** to visualize the build:

```bash
putup show graph --summary         # text overview
putup show graph | dot -Tpng -o deps.png   # graphviz visualization
```

For the full reference, see <https://github.com/typeless/putup/blob/main/docs/reference.md>.
