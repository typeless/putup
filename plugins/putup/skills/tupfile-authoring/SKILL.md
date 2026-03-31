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

**Conditional source pattern** -- append sources guarded by config:

```tup
srcs += always.c
srcs-@(FEATURE_FOO) += foo.c
srcs-@(FEATURE_BAR) += bar.c

: foreach $(srcs) $(srcs-y) |> !cc |> {objs}
```

Tup deduplicates inputs, so listing a file under multiple config guards is safe.

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

## 10. Debugging Build Failures

**Read the first error.** putup prints errors in dependency order.
Fix the first one -- later errors often cascade from it.

**Parse without building** to check syntax:

```bash
putup parse -v           # verbose: shows each Tupfile as parsed
putup parse --strict     # check dual-mode composability conventions
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
