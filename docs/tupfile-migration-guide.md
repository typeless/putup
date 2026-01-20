# Tupfile Migration Guide

Reference notes from migrating Busybox (1789 commands) from Kbuild/Make to Tup.

---

## Project Structure

```
project/
├── Tupfile.ini          # Empty file - marks project root for tup
├── Tuprules.tup         # Shared variables and macros
├── tup.config           # Build configuration (CONFIG_XXX=y)
├── Tupfile              # Top-level link rule
├── include/Tupfile      # Generated headers
└── subsystem/
    └── Tupfile          # Per-directory build rules
```

## Configuration: tup.config

Config file at project root. Variables accessed via `@(NAME)` syntax:

```ini
# tup.config
CONFIG_FEATURE_FOO=y
CONFIG_DEBUG=n
CONFIG_VERSION=1.0
```

```tup
# In Tupfile
ifeq (@(DEBUG),y)
CFLAGS += -g
endif
```

---

## Idiomatic Patterns

### Pattern 1: Conditional Sources with `srcs-@(CONFIG)`

```tup
# Unconditional
srcs += always_built.c

# Conditional on single config
srcs-@(FEATURE_FOO) += foo.c

# Same source enabled by multiple features (tup deduplicates)
srcs-@(FEATURE_A) += shared.c
srcs-@(FEATURE_B) += shared.c
srcs-@(FEATURE_C) += shared.c
```

**Key insight**: Tup automatically deduplicates, so listing the same file multiple times is safe and idiomatic.

### Pattern 2: Negative Conditionals (Exclusion)

When a "small" option replaces individual options:

```tup
# MODPROBE_SMALL provides all module utils
srcs-@(MODPROBE_SMALL) += modprobe-small.c

# Individual utils only when MODPROBE_SMALL is NOT enabled
ifneq (@(MODPROBE_SMALL),y)
srcs-@(INSMOD) += insmod.c
srcs-@(RMMOD) += rmmod.c
srcs-@(INSMOD) += modutils.c
srcs-@(RMMOD) += modutils.c
endif
```

### Pattern 3: Assembly Files

Separate variable for assembly (different compilation flags):

```tup
srcs += regular.c
asm_srcs += accelerated.S

: foreach $(srcs) $(srcs-y) | <gen-headers> |> !cc |> {objs}
: foreach $(asm_srcs) | <gen-headers> |> ^ AS %b^ $(CC) $(CFLAGS) -c %f -o %o |> %B.o {objs}
: {objs} |> !ar |> lib.a
```

### Pattern 4: Order-Only Groups for Generated Headers

Headers generated at build time must be available before compilation:

```tup
# include/Tupfile - generates headers
: .config Config.in |> conf -s Config.in |> autoconf.h <gen-headers>
: .config |> scripts/gen_build_files.sh |> applets.h usage.h <gen-headers>

# subsystem/Tupfile - depends on generated headers
: foreach $(srcs) | $(S)/include/<gen-headers> |> !cc |> {objs}
```

**The `|` separator**: Inputs after `|` are order-only (must exist before command runs, but changes don't trigger rebuild).

### Pattern 5: Cross-Directory Library Aggregation

```tup
# subsystem/Tupfile - outputs to shared group
: {objs} |> !ar |> lib.a $(S)/<libs>

# root/Tupfile - consumes from group
: <libs> |> $(CC) -Wl,--start-group %f -Wl,--end-group -o %o |> binary
```

---

## Tuprules.tup Template

```tup
# S/B Convention (Yocto-inspired):
#   S = source root path (auto-computed via TUP_CWD during includes)
#   B = build root path (derived from TUP_VARIANT_OUTPUTDIR)
#
# Subdirectory Tupfiles just need: include_rules
S = $(TUP_CWD)
B = $(TUP_VARIANT_OUTPUTDIR)/$(S)

# Toolchain (importable from environment for cross-compilation)
import CC=gcc
import HOSTCC=gcc
import AR=ar

# Export for subprocess environment (scripts that need these)
export HOSTCC
srctree = .
export srctree

# Flags
# Use $(S) for source tree paths, $(B) for build tree paths (generated headers)
CFLAGS = -Wall -Os
CPPFLAGS = -I$(B)/include -I$(S)/include -include $(B)/include/autoconf.h

# Optional config-based flags
ifeq (@(DEBUG),y)
CFLAGS += -g -O0
endif

ifeq (@(STATIC),y)
LDFLAGS += -static
endif

# Bang macros
# Order-only dependency on gen-headers (path relative to source root)
!cc = | $(S)/include/<gen-headers> |> ^ CC %b^ $(CC) $(CFLAGS) $(CPPFLAGS) -c %f -o %o |> %B.o
!ar = |> ^ AR %o^ $(AR) rcs %o %f |>
!hostcc = |> ^ HOSTCC %b^ $(HOSTCC) -Wall -O2 -c %f -o %o |> %B.o
```

### S/B Convention Explained

The `S` and `B` variables automatically compute paths to source and build roots:

- **S (Source root)**: Uses `TUP_CWD` which changes during includes to give the path back to the directory containing Tuprules.tup
- **B (Build root)**: Combines `TUP_VARIANT_OUTPUTDIR` (path to variant output) with `S` to get the build tree root

**Example from `src/lib/Tupfile`** (with `-B build` variant):
- `TUP_CWD` = `../..` (path from src/lib to root, computed during include)
- `S` = `../..`
- `TUP_VARIANT_OUTPUTDIR` = `../../build/src/lib`
- `B` = `../../build/src/lib/../..` = `../../build`

This eliminates the need for manual `TOROOT` or `ROOT` variables in each subdirectory.

---

## Generated Headers Pattern

For projects with code generation (config headers, table generators):

```tup
# include/Tupfile

# 1. Generate config header from kconfig
: $(S)/.config $(S)/Config.in |> ^ GEN autoconf.h^ cd $(S) && scripts/conf -s Config.in |> autoconf.h <gen-headers>

# 2. Build host tool that generates tables
: $(S)/tools/gen_tables.c | autoconf.h |> ^ HOSTCC gen_tables^ $(HOSTCC) -I. %f -o %o |> gen_tables

# 3. Run host tool to generate header
: gen_tables |> ^ GEN tables.h^ ./gen_tables %o |> tables.h <gen-headers>

# 4. Scripts that need environment variables
: $(S)/.config | autoconf.h |> ^ GEN embedded.h^ cd $(S) && srctree=. scripts/gen_embedded include/embedded.h |> embedded.h <gen-headers>
```

**Common pitfall**: Scripts that use `$srctree` or similar environment variables need explicit assignment in the tup rule.

---

## Kbuild to Tupfile Conversion

### Kbuild Syntax -> Tup Syntax

| Kbuild | Tup |
|--------|-----|
| `lib-y += foo.o` | `srcs += foo.c` |
| `lib-$(CONFIG_FOO) += bar.o` | `srcs-@(FOO) += bar.c` |
| `obj-y += subdir/` | (separate Tupfile in subdir) |
| `EXTRA_CFLAGS += -DFOO` | `CFLAGS += -DFOO` |

### Automated Conversion Script

See `test-busybox/scripts/kbuild2tupfile.sh` for AWK-based converter that:
- Parses `lib-y +=` and `lib-$(CONFIG_X) +=` patterns
- Outputs `srcs +=` and `srcs-@(X) +=`
- Handles backslash line continuations
- Generates standard build rules

### Manual Fixes Required After Conversion

1. **Assembly files**: Converter assumes `.o` -> `.c`, but assembly needs `.S`
2. **Negative conditionals**: `ifneq` patterns not expressible in simple converter
3. **Shared source groups**: Complex Kbuild patterns may need manual review
4. **Environment variables**: Scripts needing `srctree`, etc.

---

## Debugging Tips

### Missing Symbol Errors

Check that all required source files are included. Common issues:
- Shared sources missing from unconditional list
- Conditional group missing files (e.g., dpkg_srcs missing a file)

### Circular Dependencies

Tup detects cycles. If you see cycle errors:
- Check `<group>` references don't create loops
- Generated headers must be in order-only position (`|`)

### Config Not Taking Effect

- Verify config name matches exactly (case-sensitive)
- Use `@(NAME)` not `$(CONFIG_NAME)` for tup.config values
- Check for typos in `ifeq`/`ifneq` conditions

---

## Validation Checklist

```bash
# Clean build
rm -rf .pup **/*.o **/*.a output_binary
putup

# Verify binary works
./output_binary --help

# Check command count matches expectations
# (Busybox: 1789 commands for full config)
```

---

## Files Reference (Busybox Migration)

| File | Purpose |
|------|---------|
| `Tuprules.tup` | Compiler flags, bang macros, import/export |
| `tup.config` | 400+ CONFIG_XXX options |
| `Tupfile` | Top-level link rule with `<libs>` group |
| `include/Tupfile` | Generated headers (autoconf.h, applets.h, etc.) |
| `scripts/kconfig/Tupfile` | Build kconfig conf tool as host binary |
| `libbb/Tupfile` | Core library with assembly files |
| `modutils/Tupfile` | Example of negative conditional pattern |
| `archival/libarchive/Tupfile` | Example of shared source groups |
| `scripts/kbuild2tupfile.sh` | Automated converter |
