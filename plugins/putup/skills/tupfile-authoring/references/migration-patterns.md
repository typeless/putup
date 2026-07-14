# Migration Patterns: Kbuild/Make to Tupfile

Patterns for converting Kbuild and Makefile projects to Tupfiles,
drawn from migrating Busybox (1789 commands).

## Kbuild Syntax Mapping

| Kbuild | Tupfile |
|--------|---------|
| `lib-y += foo.o` | `srcs += foo.c` |
| `lib-$(CONFIG_FOO) += bar.o` | `srcs-@(FOO) += bar.c` |
| `obj-y += subdir/` | Separate `Tupfile` in subdir |
| `EXTRA_CFLAGS += -DFOO` | `CFLAGS += -DFOO` |

Note: Kbuild references `.o` files; Tupfiles reference `.c` sources.

## Tuprules.tup Template

```tup
# S/B Convention:
#   S = relative path back to source root (auto-computed via TUP_CWD)
#   B = corresponding path into the build tree
S = $(TUP_CWD)
B = $(TUP_VARIANT_OUTPUTDIR)/$(S)

# Toolchain (importable for cross-compilation)
import CC=gcc
import HOSTCC=gcc
import AR=ar

# Export for subprocesses that need environment variables
export HOSTCC
srctree = .
export srctree

# Flags -- use $(S) for source paths, $(B) for generated-file paths
CFLAGS = -Wall -Os
CPPFLAGS = -I$(B)/include -I$(S)/include -include $(B)/include/autoconf.h

ifeq (@(DEBUG),y)
  CFLAGS += -g -O0
endif

ifeq (@(STATIC),y)
  LDFLAGS += -static
endif

# Bang macros
!cc = | $(S)/include/<gen-headers> |> ^ CC %b^ $(CC) $(CFLAGS) $(CPPFLAGS) -c %f -o %o |> %B.o
!ar = |> ^ AR %o^ $(AR) rcs %o %f |>
!hostcc = |> ^ HOSTCC %b^ $(HOSTCC) -Wall -O2 -c %f -o %o |> %B.o
```

### S/B Convention Explained

`S` and `B` give every Tupfile a stable path to the source and build roots
without hardcoding `../../..` chains.

From `src/lib/Tupfile` with `-B build`:
- `TUP_CWD` = `../..` (path from src/lib back to root during include)
- `S` = `../..`
- `TUP_VARIANT_OUTPUTDIR` = `../../build/src/lib`
- `B` = `../../build/src/lib/../..` = `../../build`

## Conditional Sources Pattern

```tup
srcs += always_built.c

# Single config guard
srcs-@(FEATURE_FOO) += foo.c

# Multiple guards on same source (tup deduplicates)
srcs-@(FEATURE_A) += shared.c
srcs-@(FEATURE_B) += shared.c

# Negative conditional (exclusion)
srcs-@(MODPROBE_SMALL) += modprobe-small.c
ifneq (@(MODPROBE_SMALL),y)
  srcs-@(INSMOD) += insmod.c
  srcs-@(RMMOD) += rmmod.c
endif

: foreach $(srcs) $(srcs-y) | <gen-headers> |> !cc |> {objs}
```

## Assembly Files

Keep assembly in a separate variable with different flags:

```tup
srcs += regular.c
asm_srcs += accelerated.S

: foreach $(srcs) $(srcs-y) | <gen-headers> |> !cc |> {objs}
: foreach $(asm_srcs) | <gen-headers> |> ^ AS %b^ $(CC) $(CFLAGS) -c %f -o %o |> %B.o {objs}
: {objs} |> !ar |> lib.a
```

## Generated Headers

```tup
# include/Tupfile

# Generate config header
: $(S)/.config $(S)/Config.in |> ^ GEN autoconf.h^ \
  cd $(S) && scripts/conf -s Config.in |> autoconf.h <gen-headers>

# Build host tool
: $(S)/tools/gen_tables.c | autoconf.h |> ^ HOSTCC gen_tables^ \
  $(HOSTCC) -I. %f -o %o |> gen_tables

# Run host tool to produce header
: gen_tables |> ^ GEN tables.h^ ./gen_tables %o |> tables.h <gen-headers>
```

Scripts that use environment variables like `$srctree` need explicit
assignment in the tup rule -- tup does not inherit the outer environment
unless `export` is used.

## Cross-Directory Library Aggregation

```tup
# subsystem/Tupfile -- output to shared group
: {objs} |> !ar |> lib.a $(S)/<libs>

# root/Tupfile -- link from group
: <libs> |> $(CC) -Wl,--start-group %f -Wl,--end-group -o %o |> binary
```

## Manual Fixes After Automated Conversion

1. **Assembly files** -- converter assumes `.o` maps to `.c`; fix `.S` sources
2. **Negative conditionals** -- `ifneq` patterns need manual review
3. **Shared source groups** -- complex Kbuild patterns may need deduplication
4. **Environment variables** -- scripts needing `srctree` etc. need `export`

## Validation

```bash
putup parse -v            # check syntax
putup parse --check=error # check composability conventions (alias: --strict)
putup                     # full build
putup show instructions   # verify command count matches expectations
```

For the full reference, see <https://github.com/typeless/putup/blob/main/docs/reference.md>.
