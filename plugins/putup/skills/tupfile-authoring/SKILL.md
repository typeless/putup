---
name: tupfile-authoring
description: Writing Tupfiles with putup. Use when authoring Tupfiles, debugging build failures, working with rule syntax, variables, bang macros, groups, conditionals, or generators.
---

# Tupfile Authoring

Guide for writing Tupfiles with putup. For the full manual, see
<https://github.com/typeless/putup/blob/main/docs/reference.md>.

## 1. Rule Syntax

```
: [foreach] [inputs] [| order-only] |> command |> [outputs] [<order-group>] [| extra-outputs] [{group}]
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
| `%d` | Name of the Tupfile's directory | `src` in `src/Tupfile` |
| `%e` | Extension (foreach rules only, file must have one) | `c` |
| `%g` | Glob match portion (one input, produced by a glob) | `foo` from `*_test.c` + `foo_test.c` |
| `%i` | Order-only inputs (after `\|`) | `z.h y.h` |
| `%O` | The single output without its extension | `sub/foo` from `sub/foo.so` (in-tree; under `-B` the path carries the build-dir prefix) |
| `%%` | Literal `%` | `%` |

Numbered variants (`%1f`, `%2o`, `%1i`) select a specific input, output, or
order-only input:

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
| `:=` | Define | Same as `=`; the value is expanded at assignment |
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
```

There is no default syntax inside `@()`; default an unset or empty value
with `ifeq`:

```tup
DEBUG = @(DEBUG)
ifeq ($(DEBUG),)
  DEBUG = n
endif
```

The full-prefix forms `@(CONFIG_NAME)` and `$(CONFIG_NAME)` also read
tup.config. A Tupfile assignment to a `CONFIG_` name shadows the tup.config
value for `$(CONFIG_NAME)` only; `@()` always reads tup.config. Kernel-style
projects use `$(CONFIG_NAME)` to compose variable names:

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
`^ TEXT^` at the start of the command; a space must follow the opening `^`.
A line continuation before or after it is fine:

```tup
: input |> ^ CC %b^ $(CC) -c %f -o %o |> output.o

: input |> ^ CC %b^ \
  $(CC) -c %f -o %o |> output.o

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
`export` passes the environment variable of that name through to commands;
it does not pass a Tupfile variable's value.

```tup
import CC=gcc               # read $CC from env, default to gcc
import CROSS_COMPILE=       # read $CROSS_COMPILE, default empty
export PKG_CONFIG_PATH      # pass $PKG_CONFIG_PATH from putup's environment
```

Commands get a minimal environment (`PATH`, `HOME`, `TMPDIR`); any other
environment variable a command reads must be `export`ed. To hand a Tupfile
value to a script, set it in the command: `srctree=$(S) $(S)/scripts/conf %o`.

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
- Explicit file lists are more maintainable for larger directories

Exclude files from a glob with `^` followed by a regular expression:

```tup
: foreach *.c ^b.*\.c |> !cc |> {objs}   # skips b1.c, bb.c
```

Use globs for small stable directories. Use explicit lists when files change
frequently.

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
| `command not found` | `$(CC)` undefined, or `$CC` written where `$(CC)` was meant |
| Output written to wrong directory | Missing `cd $(TUP_VARIANT_OUTPUTDIR)` for generators |
| `No tup.config found` | Run `putup configure` first |
| Circular dependency | `<group>` references forming a loop |
| `Failed to find directory ID for dir '...'` | An input or glob names a directory no source or rule provides; check the path, e.g. `$(S)/dir/<group>` |
| Variable has unexpected value | Run `putup show var NAME` to see assignment history |

**Generate a dependency graph** to visualize the build:

```bash
putup show graph --summary         # text overview
putup show graph | dot -Tpng -o deps.png   # graphviz visualization
```

## 15. What putup Refuses

Each of these stops the parse with an error:

| Rule | Error |
|------|-------|
| Input or glob into a directory nothing provides | `Failed to find directory ID for dir 'nodir/a.c'` |
| Unknown percent flag | `Unknown %-flag: 'z'` |
| `{bin}` input no earlier rule declares | `Unable to find bin 'nobin'` |
| Output onto an existing directory | `Attempting to insert 'sub' as a generated node when it already exists as a different type (directory)` |
| `%e` outside a foreach rule over a file with an extension | `%e is only valid with a foreach rule for files that have extensions` |
| Caret flags (`^c TEXT^`) | `Caret flags are not supported; a display annotation must begin with a space` |
| `{bin}` not last in the output list | `bin must be at the end of the output list` |
| Output above the build root or at an absolute path | `Unable to write to a file outside of the build hierarchy: ../escape.txt` |

For the full reference, see <https://github.com/typeless/putup/blob/main/docs/reference.md>.
