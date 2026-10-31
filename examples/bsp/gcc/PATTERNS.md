# Putup Patterns for Large Multi-Library Projects

Lessons learned building GCC cc1 with putup in 3-tree mode (`-C config -S source -B build`).

## 3-Tree Variable Resolution

In 3-tree mode, two key variables drive path resolution:

- `$(S)` — relative path from the current Tupfile to the **config root** (set via `TUP_CWD` in root `Tuprules.tup`)
- `$(B)` — relative path from the source directory to the **build root** (via `$(TUP_VARIANT_OUTPUTDIR)/$(S)`)
- `$(TUP_VARIANT_OUTPUTDIR)` — path from the current source dir to its build output dir

Commands run from the **source directory**, not the build directory. Input files resolve against the source tree. Output files go to the build tree.

### Path examples for `gcc/Tupfile`:

| Variable | Value | Resolves to |
|----------|-------|-------------|
| `$(S)` | `..` | Config root (relative from gcc/) |
| `$(B)` | `../../pup/build-gcc` | Build root |
| `$(SRC)` = `$(S)/$(GCC_DIR)` | `../gcc` | Source gcc/ dir |
| `$(TUP_VARIANT_OUTPUTDIR)` | `../../pup/build-gcc/gcc` | Build gcc/ dir |

## Config File Paths (`!gen-config`)

Each library generates `config.h` from its subdir's `tup.config`. The common pattern:

```tup
# WRONG: reads root tup.config (only has CC/CXX/AR)
!gen-config = |> awk ... $(B)/tup.config > %o |>

# RIGHT: reads library's own tup.config
!gen-config = |> awk ... $(B)/$(LIBRARY_DIR)/tup.config > %o |>
```

This works in both modes:
- **Standalone** (`cd gmp && putup`): `$(B)` = variant output root, `$(LIBRARY_DIR)` = `.`, path = `variant/./tup.config`
- **Composed** (root build): `$(B)` = build root, `$(LIBRARY_DIR)` = `gmp`, path = `build_root/gmp/tup.config`

## Config-Tree Files as Inputs

In 3-tree mode, input files in rules resolve against the **source tree**. Files that exist only in the config tree (alongside Tupfiles) **cannot be referenced as rule inputs**.

Solutions:
1. **Generate inline** — Use tup variables and shell commands to produce the content directly
2. **Generate from source** — If the file exists in the source tree, reference it there

Example: `gtyp-input.list` uses inline generation with tup variable expansion:

```tup
gtyp_src = input.h coretypes.h bitmap.h ...

: |> ^ GEN %o^ { \
  for f in $(gtyp_src); do printf '%s\n' "$(SRC)/$f"; done; \
  } > %o |> gtyp-input.list
```

`$(SRC)` is expanded at parse time, producing correct relative paths in the command.

## Shell Variables in Commands

Tup only expands `$(VAR)` and `@(VAR)` patterns. Bare `$var` is passed through to the shell:

```tup
# $f is a shell variable — tup doesn't touch it
: |> for f in $(my_list); do echo "$f"; done > %o |> output
```

## Configure-Generated Wrappers

GCC's autoconf generates several wrapper headers. In putup builds, generate them as rules:

| Header | Content | Used by |
|--------|---------|---------|
| `auto-host.h` | `#define` from `tup.config` | gcc/ (via `!gen-config`) |
| `bconfig.h` | `#include "auto-host.h"` | Generators |
| `config.h` | Guards + `#include "auto-host.h"` | Target objects |
| `gstdint.h` | `#include <stdint.h>` | libdecnumber |

## Host Config Pitfalls (gcc/tup.config)

GCC's configure detects host properties. Key entries for x86_64 Linux:

- `SIZEOF_*` defines are sufficient; `HOST_BITS_PER_*` is derived by `hwint.h` (don't duplicate)
- `INT64_T_IS_LONG=1` — required for correct `HOST_WIDE_INT` type on x86_64
- `HAVE_STRSIGNAL=1` — prevents conflicting `strsignal` declaration in `system.h`
- `HAVE_DECL_BASENAME=1` — prevents conflicting `basename` declaration in `libiberty.h`

## Self-Contained Library Convention

Each library has its own `Tuprules.tup` with `?=` defaults:

```tup
S ?= $(TUP_CWD)           # Standalone: config root is here
B ?= $(TUP_VARIANT_OUTPUTDIR)/$(S)
CC ?= gcc                   # Standalone default
GMP_DIR ?= .                # Standalone: I am the root

!gen-config = |> awk ... $(B)/$(GMP_DIR)/tup.config > %o |>
```

Root `Tuprules.tup` sets layout vars (`GMP_DIR = gmp`, etc.) — `?=` in libs become no-ops. CFLAGS and bang macros can be unprefixed since each lib's scope is isolated.
