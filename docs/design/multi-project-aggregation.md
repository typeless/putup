# Multi-Project Aggregation Guide

> **Status: Work in Progress** - This document captures design exploration for multi-project builds. The "Current Pattern" section works today; "Future Enhancement" describes proposed improvements.

## Overview

Pattern for aggregating multiple independent Tupfile-based projects (e.g., Linux, U-Boot, Busybox) under a single pup instance.

**Document structure:**
- **Current Pattern** - Works today with existing tup/pup features (requires subproject cooperation)
- **Future Enhancement** - Tupfile-based configuration for simpler, more consistent builds

---

# Current Pattern (Existing Features)

## Directory Structure

```
firmware/
├── Tupfile.ini                  # Root project marker
├── Tuprules.tup                 # CONFIG_LAYER, common macros
│
├── configs/                     # Config layers (Tupfile syntax, includable)
│   ├── common.tup               # Shared settings
│   ├── linux.tup                # Linux-specific
│   └── uboot.tup                # U-Boot-specific
│
├── linux/                       # Subproject (git submodule, cooperative)
│   ├── Tupfile.ini              # Ignored by parent project
│   ├── Tuprules.tup             # include_rules, then include configs
│   └── kernel/
│       └── Tupfile
│
├── uboot/
│   ├── Tupfile.ini              # Ignored
│   ├── Tuprules.tup
│   └── Tupfile
│
├── archive/
│   └── Tupfile                  # Final packaging
│
└── build/                       # Out-of-tree build (-B flag)
    ├── .pup/                    # Output root marker
    └── tup.config               # Variant selection (MACHINE, DISTRO)
```

## Two Parsers (Current Limitation)

| File | Syntax | Parser | Include-able |
|------|--------|--------|--------------|
| `tup.config` | Config syntax (`CONFIG_X = y`) | Config parser | No |
| `*.tup` | Tupfile syntax (`X = y`) | Tupfile parser | Yes |

tup/pup have separate parsers. Config layer files use Tupfile syntax so they can be included.

**Variable bridging:**
- `tup.config`: `CONFIG_MACHINE = board-xyz` → accessible as `@(MACHINE)`
- Config layer: `MACHINE = @(MACHINE)` → copies to regular variable for `$(MACHINE)`

## Configuration Flow

### build/tup.config (variant entry point)

```
# Minimal - just selects what to build
CONFIG_MACHINE = board-xyz
CONFIG_DISTRO = router
```

### Tuprules.tup (project root)

```tup
# Config layer path - $(TUP_CWD) resolves correctly at any depth
CONFIG_LAYER = $(TUP_CWD)/configs

# Bridge @() to $() for use in includes
MACHINE = @(MACHINE)
DISTRO = @(DISTRO)
```

### configs/common.tup

```tup
# Shared settings (Tupfile syntax)
ARCH = arm
CROSS_COMPILE = arm-linux-gnueabihf-
```

### configs/linux.tup

```tup
# Linux-specific (Tupfile syntax)
LINUX_DEFCONFIG ?= defconfig
LINUX_EXTRA_CFLAGS = -Os
```

### linux/Tuprules.tup (cooperative subproject)

```tup
# Inherit CONFIG_LAYER from root
include_rules

# Include configs (order matters - later overrides earlier)
include $(CONFIG_LAYER)/common.tup
include $(CONFIG_LAYER)/linux.tup

# Subproject defaults (config can override via ?= above)
LINUX_SRC = $(TUP_CWD)

# Build macro
!linux_make = |> ^ MAKE %o^ make -C $(LINUX_SRC) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) |>
```

### linux/kernel/Tupfile

```tup
include_rules

: |> !linux_make $(LINUX_DEFCONFIG) |>
: |> !linux_make vmlinux |> vmlinux | <linux-vmlinux>
: |> !linux_make modules |> <linux-modules>
```

## Cross-Project Dependencies

### Using Groups

```tup
# linux/kernel/Tupfile - export output
: |> !linux_make vmlinux |> vmlinux | <linux-vmlinux>

# archive/Tupfile - consume outputs
: <linux-vmlinux> <uboot-bin> |> zip -j firmware.zip %f |> firmware.zip
```

### Staging Directory Pattern

```tup
# linux/Tupfile - install to staging
: vmlinux |> cp %f $(STAGING_DIR)/boot/ |> $(STAGING_DIR)/boot/vmlinux

# archive/Tupfile - package from staging
: $(STAGING_DIR)/boot/* |> tar -czf %o -C $(STAGING_DIR) . |> firmware.tar.gz
```

## Build Commands

```bash
# Initial build
pup -B build

# Rebuild specific subproject
pup build/linux/

# Clean and rebuild
pup clean -B build && pup -B build
```

## Key Points

1. **Single pup index** - All subprojects share one `.pup/` for maximum parallelism
2. **Nested Tupfile.ini ignored** - Subprojects can be standalone but are aggregated here
3. **Config layers use Tupfile syntax** - `.tup` files, includable, full language features
4. **Explicit includes** - Each subproject's Tuprules.tup includes its config
5. **Groups for dependencies** - `<group>` syntax connects subproject outputs
6. **`?=` for defaults** - Config can override, subprojects have fallbacks
7. **CONFIG_LAYER in Tuprules.tup** - `$(TUP_CWD)` ensures correct path at any depth

## Limitations

1. **Requires subproject cooperation** - Subprojects must `include` config layers
2. **Unmodified subprojects using `@()` won't work** - They expect their own tup.config
3. **Config conflicts resolved by include order** - Last wins
4. **Two parsers** - tup.config vs Tupfile syntax requires bridging with `MACHINE = @(MACHINE)`

---

# Future Enhancement: Full Tupfile-Based Configuration

## Goal

Eliminate the two-parser problem. Use Tupfile syntax everywhere, including variant selection.

## What Changes

| Aspect | Current | Future |
|--------|---------|--------|
| Variant selection | `tup.config` (config parser) | Tupfile syntax |
| Variable access | `@(VAR)` vs `$(VAR)` | `$(VAR)` only |
| Bridging needed | `MACHINE = @(MACHINE)` | No bridging |
| Unmodified subprojects | Won't work (need `@()`) | Need nestable config (see below) |

## Hierarchical Config Layers

```
firmware/
├── Tupfile.ini
├── Tuprules.tup                 # CONFIG_LAYER, bridges variant selection
│
├── configs/
│   ├── arch/
│   │   ├── arm-v7.tup
│   │   └── arm-v8.tup
│   ├── soc/
│   │   └── soc-family-x.tup     # includes arch/arm-v7.tup
│   ├── machine/
│   │   └── board-xyz.tup        # includes soc/soc-family-x.tup
│   └── distro/
│       └── router.tup
│
├── linux/
│   └── Tuprules.tup             # includes machine + distro configs
│
└── build/
    ├── .pup/
    └── tup.config               # CONFIG_MACHINE=board-xyz (still needed for now)
```

## Config Composition

Configs include other configs - machine includes soc, soc includes arch:

```tup
# configs/arch/arm-v7.tup
ARCH = arm
CROSS_COMPILE = arm-linux-gnueabihf-
CFLAGS += -march=armv7-a
```

```tup
# configs/soc/soc-family-x.tup
include $(CONFIG_LAYER)/arch/arm-v7.tup
SOC_FAMILY = x
KERNEL_LOADADDR = 0x40000000
```

```tup
# configs/machine/board-xyz.tup
include $(CONFIG_LAYER)/soc/soc-family-x.tup
MACHINE = board-xyz
KERNEL_DEFCONFIG = board_xyz_defconfig
# Override SOC default:
KERNEL_LOADADDR = 0x42000000
```

## Subproject Integration

```tup
# linux/Tuprules.tup
include_rules
include $(CONFIG_LAYER)/machine/$(MACHINE).tup
include $(CONFIG_LAYER)/distro/$(DISTRO).tup

DEFCONFIG ?= defconfig

!linux_make = |> ^ MAKE %o^ make ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) |>
```

## Rebuild Tracking

Included `.tup` files are automatically tracked as dependencies:
- Change `configs/arch/arm-v7.tup` → pup re-parses, rebuilds affected targets
- No manual refresh needed

## Remaining Problem: Unmodified Subprojects

Subprojects using `@(DEFCONFIG)` still need per-subproject config resolution. Options:

1. **Nestable tup.config** (requires pup enhancement)
   - `build/linux/tup.config` provides `CONFIG_DEFCONFIG` for linux/ subtree
   - Subproject's `@(DEFCONFIG)` finds it without modification

2. **Wrapper Tuprules.tup** (works today, requires adding file)
   - Add `linux/Tuprules.tup` that bridges: `DEFCONFIG = $(LINUX_DEFCONFIG)`
   - Not truly "unmodified" but minimal change

## Key Points

1. **Tupfile syntax for configs** - One parser, full language, tracked dependencies
2. **Hierarchical includes** - machine → soc → arch composition
3. **tup.config still needed** - For variant selection (`CONFIG_MACHINE`)
4. **Unmodified subprojects** - Still requires nestable tup.config enhancement
