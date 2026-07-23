---
name: composable-libraries
description: Writing dual-mode Tupfiles for standalone and component builds. Use when writing reusable libraries, using the self-contained library convention, setting up multi-library projects, or running putup parse --check / --strict.
---

# Composable Libraries

Write Tupfiles that build both standalone (`cd libfoo && putup -B build`) and as part of a larger project (`cd big-project && putup -B build`) with zero changes.

Full reference: <https://github.com/typeless/putup/blob/main/docs/reference.md>

## The Problem

Every Tuprules.tup sets `S = $(TUP_CWD)` to anchor source paths. When a component uses `=`, it overwrites the parent's value during `include_rules` (root-to-leaf merge), breaking all `$(S)/...` references in composed mode.

```tup
# Root Tuprules.tup
S = $(TUP_CWD)       # S = ../..  (from mpfr/src/)

# mpfr/Tuprules.tup
S = $(TUP_CWD)       # OVERWRITES S = ..  -- root's value lost!
```

## The Convention

**Root uses `=`** (it IS the authority). **Components use `?=`** (set only if undefined):

```tup
# Root Tuprules.tup — authoritative assignments
S = $(TUP_CWD)
B = $(TUP_VARIANT_OUTPUTDIR)/$(S)
CC = @(CC)
AR = @(AR)
GMP_DIR = gmp
MPFR_DIR = mpfr
MPC_DIR = mpc
```

```tup
# mpfr/Tuprules.tup — soft defaults for standalone builds
S ?= $(TUP_CWD)
B ?= $(TUP_VARIANT_OUTPUTDIR)/$(S)
CC ?= gcc
AR ?= ar
GMP_DIR ?= ../gmp
MPFR_DIR ?= .

CFLAGS  = -O2 -DHAVE_CONFIG_H
CFLAGS += -I$(S)/$(MPFR_DIR)/src
CFLAGS += -I$(S)/$(GMP_DIR)

!cc = | $(S)/$(GMP_DIR)/<gen-headers> |> ^ CC %b^ $(CC) $(CFLAGS) -c %f -o %o |> %B.o
```

## How include_rules Merges

`include_rules` includes every `Tuprules.tup` from project root down to the current directory. For `mpfr/src/Tupfile`:

1. Root `Tuprules.tup` runs first: sets `S`, `B`, `CC`, `GMP_DIR = gmp`, `MPFR_DIR = mpfr`
2. `mpfr/Tuprules.tup` runs second: `?=` defaults are no-ops (already set), but `CFLAGS` and `!cc` are defined fresh

This layering means root controls the layout while each component controls its own build flags.

## Prefixed DIR Variables

A library that depends on another needs to reference both directories in the same file:

```tup
# mpc/Tuprules.tup
CFLAGS += -I$(S)/$(GMP_DIR)
CFLAGS += -I$(S)/$(MPFR_DIR)/src
CFLAGS += -I$(S)/$(MPC_DIR)/src
```

Three different paths need three different names. The root sets all of them in one shared scope. A single `DIR` would collide.

**Default to `.` for standalone use:**

```tup
# In component Tuprules.tup
GMP_DIR ?= ../gmp
MPFR_DIR ?= .
```

## Unprefixed CFLAGS and Bang Macros

`CFLAGS` and `!cc` do NOT need prefixes. Each component's `Tuprules.tup` is only included by Tupfiles in its own subtree via `include_rules`. There is no shared scope where they could collide.

```tup
# gmp/Tuprules.tup
CFLAGS = -O2 -I$(S)/$(GMP_DIR)
!cc = |> ^ CC %b^ $(CC) $(CFLAGS) -c %f -o %o |> %B.o

# mpfr/Tuprules.tup (different CFLAGS, no collision)
CFLAGS = -O2 -I$(S)/$(MPFR_DIR)/src -I$(S)/$(GMP_DIR)
!cc = | $(S)/$(GMP_DIR)/<gen-headers> |> ^ CC %b^ $(CC) $(CFLAGS) -c %f -o %o |> %B.o
```

## Verifying with `putup parse`

`putup parse` runs the convention checker and reports violations before they
cause composed-mode failures. The `--check=LEVEL` option controls severity:

```bash
putup parse                 # report violations, exit 0 (default: --check=warn)
putup parse --check=error   # report violations, exit non-zero -- for CI
putup parse --strict        # alias for --check=error
putup parse --check=none    # skip the checks
```

At the default `warn` level both errors and warnings below are printed but the
exit code stays 0; `--check=error` makes error-severity findings fail.

### Errors (fail the exit code at `--check=error`)

| Code | Rule | Example violation |
|------|------|-------------------|
| E1 | `S` must use `?=` in component Tuprules.tup | `S = $(TUP_CWD)` in mpfr/Tuprules.tup |
| E2 | `B` must use `?=` in component Tuprules.tup | `B = $(TUP_VARIANT_OUTPUTDIR)/$(S)` in mpfr/Tuprules.tup |

### Warnings (advisory; never change the exit code)

| Code | Rule | Example violation |
|------|------|-------------------|
| W1 | Toolchain vars (`CC`, `CXX`, `AR`, etc.) should use `?=` in components | `CC = gcc` in mpfr/Tuprules.tup |
| W2 | Component directories should contain `Tupfile.ini` for standalone builds | mpfr/ missing Tupfile.ini |

The tree roots are exempt -- they ARE the authority for anchor and toolchain
variables. This covers the source root and, in 3-tree builds, the config-tree root.

## Nested Project Boundaries and Composition

A subdirectory carrying its own `Tupfile.ini` is a **separate project**: the
outer build's discovery prunes it, so its Tupfiles are neither parsed nor
built. Adding the standalone marker (W2) therefore takes the component OUT of
the composed build until the outer project *depends* on it — a rule input
referencing a group (or generated file) under the nested root composes the
whole nested project back in:

```tup
# Outer Tupfile: one group reference pulls the entire gmp/ project in
: gmp/<libgmp> |> ^ gmp composed^ touch %o |> .gmp-composed
```

Composition is at project granularity — one reference anywhere under the
nested root brings its full subtree, which then builds normally. When the
outer build already consumes the component's outputs via groups (the usual
case for libraries), no extra rule is needed: the existing dependency IS the
composition. Only components nothing references (e.g. a toolchain whose final
binaries are the build's goal) need an explicit composing rule.

Explicitly targeting a path inside a nested project (`putup build/gmp`)
overrides the pruning for that subtree.

## Complete Example

### Project layout

```
project/
  Tupfile.ini
  Tuprules.tup          # Root: S=, B=, GMP_DIR=gmp, MPFR_DIR=mpfr
  tup.config
  gmp/
    Tupfile.ini          # Standalone marker
    Tuprules.tup         # S?=, B?=, GMP_DIR?=., CFLAGS, !cc
    Tupfile
    defaults.config
  mpfr/
    Tupfile.ini
    Tuprules.tup         # S?=, B?=, GMP_DIR?=../gmp, MPFR_DIR?=.
    src/
      Tupfile
    defaults.config
```

### Path resolution trace

From `mpfr/src/Tupfile` with `include_rules`:

**Composed mode** (root is `../..`):
```
Root Tuprules.tup:  S = ../..    GMP_DIR = gmp     MPFR_DIR = mpfr
mpfr/Tuprules.tup:  S ?= (no-op) GMP_DIR ?= (no-op)

$(S)/$(GMP_DIR)       = ../../gmp         (correct)
$(S)/$(MPFR_DIR)/src  = ../../mpfr/src    (correct)
```

**Standalone mode** (mpfr/ is root, root is `..`):
```
mpfr/Tuprules.tup:  S = ..    GMP_DIR = ../gmp   MPFR_DIR = .

$(S)/$(GMP_DIR)       = ../../gmp         (correct)
$(S)/$(MPFR_DIR)/src  = ../src            (correct)
```

### Scoped tup.config

Components ship defaults in a `defaults.config` file. During configure, the parent config overrides child values on collision:

```
project/tup.config         # CC=gcc, AR=ar
  gmp/defaults.config      # HAVE_ALLOCA=1
  mpfr/defaults.config     # HAVE_LOCALE=1
```

## Checklist

- [ ] Root Tuprules.tup uses `=` for `S`, `B`, toolchain, and `*_DIR` variables
- [ ] Component Tuprules.tup uses `?=` for `S`, `B`, toolchain, and `*_DIR` variables
- [ ] `*_DIR` defaults resolve correctly for standalone builds (e.g., `GMP_DIR ?= ../gmp`)
- [ ] Each component has `Tupfile.ini` for standalone mode
- [ ] Each marker-bearing component is composed by a group/file reference from the outer build (nested project roots are otherwise pruned)
- [ ] `CFLAGS` and `!cc` are unprefixed (subtree-scoped, no collision risk)
- [ ] `putup parse --check=error` reports no errors (alias: `--strict`)
- [ ] Component builds standalone: `cd gmp && putup -B build && ls build/`
- [ ] Component builds composed: `cd project && putup -B build && ls build/gmp/`
