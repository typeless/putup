# Tupfile Gotchas

Common mistakes and their fixes. Each entry includes the symptom, root cause,
and corrected code.

## Quick Reference

| Mistake | Symptom | Fix |
|---------|---------|-----|
| `$VAR` instead of `$(VAR)` | Variable not expanded; shell gets literal `$VAR` | Use `$(VAR)` for putup variables |
| Display text on continuation line | `^` appears in shell command, build error | Put `^ TEXT ^` on same line as `|>` |
| `=` instead of `?=` in component Tuprules.tup | Overrides integrator's value; `--strict` error | Use `?=` for S, B, CC, AR in components |
| `@(NAME)` with full `CONFIG_` prefix | Variable not found | Use `@(CC)`, not `@(CONFIG_CC)` |
| Missing `include_rules` | Bang macros and shared variables undefined | Add `include_rules` at top of every Tupfile |
| Output group `{name}` where order-only `<name>` needed | Unnecessary rebuild triggers | Use `<name>` for ordering-only deps |
| Generator writes to source dir | Permission error or wrong output path | `cd $(TUP_VARIANT_OUTPUTDIR)` before running generator |
| Cross-dir group without path prefix | Group not found | Use `$(S)/dir/<group>` |
| `??=` when `?=` intended | Last definition wins instead of first | `?=` = first wins; `??=` = last wins |
| Empty `ifeq` comparison without quotes | Syntax error | `ifeq (@(VAR),)` -- empty RHS is valid |

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
# WRONG -- ^ becomes part of the shell command
: input |> \
  ^ CC %b^ $(CC) -c %f -o %o |> output.o

# RIGHT -- ^ TEXT ^ immediately after |> on the same line
: input |> ^ CC %b^ \
  $(CC) -c %f -o %o |> output.o
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

Run `putup parse --strict` to catch these automatically.

### Config Variable Access

```tup
# tup.config contains: CONFIG_CC=clang

# WRONG -- @() strips CONFIG_ prefix; this looks for CONFIG_CONFIG_CC
CC = @(CONFIG_CC)

# RIGHT
CC = @(CC)

# Also works (but verbose)
CC = $(CONFIG_CC)
```

### Generator Programs in Variant Builds

```tup
# WRONG -- generator writes relative to CWD (read-only source dir)
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
