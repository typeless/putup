# Eliminate Makefile.pup Preprocessing

## Problem

The BSP example (`examples/bsp/`) uses `Makefile.pup` as a wrapper that runs
preprocessing steps before invoking putup. Two targets mutate the config tree
at `make` time:

1. **`setup-host-configs`** — copies platform-specific `configs/host-darwin/*.config`
   files over `defaults.config` files in the source tree. Mutates 9 config files
   based on the `HOST` variable.

2. **`resolve-mpn`** — runs `scripts/resolve-mpn.sh` to scan the GMP source tree
   for assembly files, then writes `gcc/gmp/mpn/defaults.config` and appends
   MPN-dependent variables to `gcc/gmp/defaults.config`.

This violates putup's design: the build system should be the single source of
truth for build logic. Preprocessing steps that mutate the config tree are
invisible to putup's dependency tracking and make builds non-reproducible.

## Design Goals

- **Zero preprocessing**: `putup configure && putup` is the complete build command
- **Config tree is immutable**: no step writes into the `-C` tree at build time
- **Platform selection via `--config`**: the only knob for platform targeting
- **Scoped config merging**: child `defaults.config` provides library defaults;
  parent `--config` overrides on collision

## Solution

### 1. Eliminate `setup-host-configs`

Different libraries need different values for the same `CONFIG_` key on darwin
(e.g., `CONFIG_HAVE_ALLOCA_H` is `n` for binutils but `1` for gmp). This means
per-library overrides cannot all live in the root platform config — scoped merge
would force one value on all libraries.

Instead, libraries with platform-specific configs get a `defaults-darwin.config`
alongside their `defaults.config`. The Tupfile selects via `ifeq`:

```tup
ifeq (@(HOST_OS),darwin)
: defaults-darwin.config |> ^ INSTALL %o^ cp %f %o |> tup.config
else
: defaults.config |> ^ INSTALL %o^ cp %f %o |> tup.config
endif
```

Only 3 libraries have meaningful differences: **binutils**, **gcc**, **libcpp**.
The other 6 libraries have identical configs across platforms (or the existing
defaults.config already works on both).

For **libbacktrace**, the only difference is `BACKTRACE_FORMAT` (elf vs macho).
This is already in the root platform config (`x86_64-linux.config`), so the
darwin platform config just needs `CONFIG_BACKTRACE_FORMAT=macho`.

**Create**: `defaults-darwin.config` in binutils/, gcc/gcc/, gcc/libcpp/
**Modify**: Those 3 Tupfiles to add `ifeq` selection
**Modify**: `darwin-x86_64-linux.config` — add `CONFIG_BACKTRACE_FORMAT=macho`
**Delete**: `examples/bsp/configs/host-darwin/` (entire directory, 9 files)

### 2. Make `resolve-mpn` a config-generating Tupfile rule

Add `CONFIG_MPN_CPU` to the platform configs (default: `generic`). The GMP mpn
Tupfile reads `@(MPN_CPU)` and runs `resolve-mpn.sh` as a config-generating rule:

```tup
# In gcc/gmp/mpn/Tupfile:
: |> ^ RESOLVE-MPN %o^ $(SCRIPTS)/resolve-mpn.sh @(MPN_CPU) $(GMP_SRC)/mpn > %o |> tup.config
```

For the parent-scope variables (`GMP_MPARAM`, `ASM_ENABLED`), the GMP Tupfile's
config rule copies `defaults.config` and appends MPN-dependent lines computed
from `@(MPN_CPU)`:

```tup
# In gcc/gmp/Tupfile:
: defaults.config |> ^ GEN %o^ cp %f %o && \
  cpu="@(MPN_CPU)"; \
  if [ -z "$cpu" ] || [ "$cpu" = "generic" ]; then \
    echo "CONFIG_GMP_MPARAM=mpn/generic/gmp-mparam.h" >> %o; \
    echo "CONFIG_NO_ASM=1" >> %o; \
  else \
    arch="${cpu%%/*}"; \
    if [ -f "mpn/$arch/gmp-mparam.h" ]; then \
      echo "CONFIG_GMP_MPARAM=mpn/$arch/gmp-mparam.h" >> %o; \
    else \
      echo "CONFIG_GMP_MPARAM=mpn/generic/gmp-mparam.h" >> %o; \
    fi; \
    echo "CONFIG_ASM_ENABLED=y" >> %o; \
  fi |> tup.config
```

**Move** `resolve-mpn.sh` stays in `scripts/` — it becomes a build-time tool
invoked by the Tupfile rule rather than a Makefile preprocessing step.

**Delete**: `examples/bsp/gcc/gmp/mpn/defaults.config` (fully generated)

**Modify**: `examples/bsp/gcc/gmp/defaults.config` — remove the 3 MPN-dependent
lines (`GMP_MPARAM`, `ASM_ENABLED`, `NO_ASM`), which are now generated.

### 3. Remove `Makefile.pup`

With preprocessing eliminated, the Makefile wrapper serves no purpose. The
canonical build command becomes:

```bash
putup configure --config configs/x86_64-linux.config -C . -S ../../source-root -B ../../build-gcc
putup -C . -S ../../source-root -B ../../build-gcc -j$(nproc)
```

Extract the download logic (`download-source`, `download-binutils`) into a
standalone `scripts/download-source.sh` for users who need to fetch tarballs.

**Delete**: `examples/bsp/Makefile.pup`

**Create**: `examples/bsp/scripts/download-source.sh` (download logic only)

### 4. Update CI

Replace `make -f Makefile.pup configure` with direct putup invocations in
`.github/workflows/ci.yml`. The `build-gcc-bsp` job becomes:

```yaml
- name: Download source
  run: cd examples/bsp && scripts/download-source.sh

- name: Configure
  run: cd examples/bsp && putup configure --config configs/x86_64-linux.config -C . -S ../../source-root -B ../../build-gcc

- name: Build
  run: cd examples/bsp && putup -C . -S ../../source-root -B ../../build-gcc -j$(nproc)
```

## File Changes

| # | File | Action |
|---|------|--------|
| 1 | `examples/bsp/Makefile.pup` | Delete |
| 2 | `examples/bsp/scripts/resolve-mpn.sh` | Keep (becomes build-time tool) |
| 3 | `examples/bsp/scripts/download-source.sh` | Create (extracted from Makefile.pup) |
| 4 | `examples/bsp/configs/host-darwin/` (9 files) | Delete directory |
| 5 | `examples/bsp/configs/darwin-x86_64-linux.config` | Modify — add backtrace override, update comments |
| 6 | `examples/bsp/configs/x86_64-linux.config` | Modify — add `CONFIG_MPN_CPU=generic` |
| 7 | `examples/bsp/binutils/defaults-darwin.config` | Create (from host-darwin/binutils.config) |
| 8 | `examples/bsp/binutils/Tupfile` | Modify — add ifeq darwin selection |
| 9 | `examples/bsp/gcc/gcc/defaults-darwin.config` | Create (from host-darwin/gcc.config) |
| 10 | `examples/bsp/gcc/gcc/Tupfile` | Modify — add ifeq darwin selection |
| 11 | `examples/bsp/gcc/libcpp/defaults-darwin.config` | Create (from host-darwin/libcpp.config) |
| 12 | `examples/bsp/gcc/libcpp/Tupfile` | Modify — add ifeq darwin selection |
| 13 | `examples/bsp/gcc/gmp/defaults.config` | Modify — remove MPN-dependent lines |
| 14 | `examples/bsp/gcc/gmp/Tupfile` | Modify — config rule computes MPN vars |
| 15 | `examples/bsp/gcc/gmp/mpn/defaults.config` | Delete |
| 16 | `examples/bsp/gcc/gmp/mpn/Tupfile` | Modify — config rule runs resolve-mpn.sh |
| 17 | `.github/workflows/ci.yml` | Modify — direct putup commands |
| 18 | `docs/reference.md` | Modify — update Makefile.pup references |
| 19 | `.claude/skills/gcc-example/SKILL.md` | Modify — update build commands |

## Verification

1. `putup configure --config configs/x86_64-linux.config -C . -S ../../source-root -B ../../build-gcc` succeeds
2. `putup -C . -S ../../source-root -B ../../build-gcc` builds successfully
3. CI `build-gcc-bsp` job passes
4. Darwin cross-compile config contains all necessary overrides
