# Tupfile Gotchas

Common mistakes and their fixes. Each entry includes the symptom, root cause,
and corrected code.

## Quick Reference

| Mistake | Symptom | Fix |
|---------|---------|-----|
| `$VAR` instead of `$(VAR)` | Variable not expanded; shell gets literal `$VAR` | Use `$(VAR)` for putup variables |
| Caret flags `^c TEXT^` | Parse error: `Caret flags are not supported` | Put a space after the opening `^` |
| `=` instead of `?=` in component Tuprules.tup | Overrides integrator's value; `--check=error` fails for S and B, warns for CC, AR | Use `?=` for S, B, CC, AR in components |
| Missing `include_rules` | Bang macros and shared variables undefined | Add `include_rules` at top of every Tupfile |
| Output group `{name}` where order-only `<name>` needed | Unnecessary rebuild triggers | Use `<name>` for ordering-only deps |
| Generator writes to source dir | `expected to write to file 'build/out.h'`; stray file in source tree | `cd $(TUP_VARIANT_OUTPUTDIR)` before running generator |
| Cross-dir group without path prefix | Group not found | Use `$(S)/dir/<group>` |
| `??=` when `?=` intended | Last definition wins instead of first | `?=` = first wins; `??=` = last wins |
| `@(VAR:-default)` | Expands to empty, even when VAR is set | `ifeq ($(VAR),)` then assign the default |
| `%f` on a `<group>` input | Command gets the literal text `<group>` | Use `%<group>` |
| GNU Make function left in ported Tupfile | Expands to empty, silently | Replace `$(notdir $<)` etc. with `%b`/`%B`/`%d` flags or shell |

## Detailed Examples

### `$(VAR)` vs `$VAR`

```tup
# WRONG -- $CC is a shell variable (probably empty)
: foo.c |> $CC -c %f -o %o |> foo.o

# RIGHT -- $(CC) is expanded by putup at parse time
: foo.c |> $(CC) -c %f -o %o |> foo.o
```

Use bare `$` intentionally when you want shell expansion:

```tup
# Intentional: capture CWD in a shell variable before cd
: |> SRCDIR=$PWD && cd $(TUP_VARIANT_OUTPUTDIR) && ./tool $SRCDIR/in |> out
```

### Display Text Placement

```tup
# WRONG -- caret flags are rejected at parse time
: input |> ^c CC %b^ $(CC) -c %f -o %o |> output.o

# RIGHT -- a space after the opening ^; a continuation before it is fine
: input |> \
  ^ CC %b^ $(CC) -c %f -o %o |> output.o
```

### Soft Set vs Weak Set

```tup
# ?= (soft set) -- first definition wins
CC ?= gcc       # CC is now "gcc"
CC ?= clang     # ignored

# ??= (weak set) -- last definition wins, applied at end of parse
OPTFLAGS ??= -O0
OPTFLAGS ??= -O2   # this wins
```

### Component Tuprules.tup Conventions

```tup
# WRONG -- unconditional = in a component overrides the integrator
S = $(TUP_CWD)
CC = gcc

# RIGHT -- ?= lets the root Tuprules.tup win when composed
S ?= $(TUP_CWD)
CC ?= gcc
```

Run `putup parse` to see these. `putup parse --check=error` (alias `--strict`) fails on `S` and `B`; `CC`, `AR` and the other toolchain variables stay warnings.

### Config Variable Access

```tup
# tup.config contains: CONFIG_CC=clang

CC = @(CC)
CC = @(CONFIG_CC)    # same value; the CONFIG_ prefix is optional
CC = $(CONFIG_CC)    # same value
```

There is no default syntax: `@(CC:-gcc)` expands to empty even when
`CONFIG_CC` is set. Default with `ifeq`:

```tup
CC = @(CC)
ifeq ($(CC),)
  CC = gcc
endif
```

### Generator Programs in Variant Builds

```tup
# WRONG -- generator writes relative to CWD (the source dir)
: input |> ./generator -o output.h |> output.h

# RIGHT -- cd to build dir first
: input |> ^ GEN %o^ SRCDIR=$PWD && \
  cd $(TUP_VARIANT_OUTPUTDIR) && \
  ./generator -o output.h $SRCDIR/input |> output.h
```

### Cross-Directory Group References

```tup
# WRONG -- unqualified group name only searches current directory
: src.c | <gen-headers> |> !cc |> src.o

# RIGHT -- prefix with path to the producing directory
: src.c | $(S)/include/<gen-headers> |> !cc |> src.o
```

### Group Members in a Command

```tup
# WRONG -- %f expands to the literal text <libs>
: <libs> |> $(CC) %f -o %o |> binary

# RIGHT -- %<libs> expands to the group's member paths
: <libs> |> $(CC) %<libs> -o %o |> binary
```

### GNU Make Functions Expand to Empty

Tupfiles ported from Make often carry Make function calls. Neither tup nor
putup supports them -- they parse as unknown variables and expand to empty
without any error:

```tup
# WRONG -- compiles with __PFILENAME__="" on every file
CFLAGS += -D__PFILENAME__=\"$(notdir $<)\"

# RIGHT -- %b is the input basename, substituted per rule
!cc = |> $(CC) $(CFLAGS) -D__PFILENAME__=\"%b\" -c %f -o %o |> %B.o
```

### `@(VAR)` vs `$(CONFIG_VAR)` Confusion

Both access config variables but differently:

```tup
# @(FOO) reads tup.config, strips CONFIG_ prefix automatically
CC = @(CC)           # reads CONFIG_CC from tup.config

# $(CONFIG_FOO) is a regular variable set by the config system
srcs-$(CONFIG_FOO) += foo.c    # expands CONFIG_FOO to y or empty
```

Both work, but mixing styles in the same project causes confusion. Pick one
convention. `$(CONFIG_VAR)` is more explicit for conditional source lists;
`@(VAR)` is more concise for value access. The kernel-style convention uses
`@(VAR)` for values and `$(CONFIG_VAR)` for conditionals.

For the full reference, see <https://github.com/typeless/putup/blob/main/docs/reference.md>.
