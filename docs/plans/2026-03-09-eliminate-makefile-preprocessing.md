# Eliminate Makefile.pup Preprocessing — Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Remove all preprocessing from the BSP example so `putup configure && putup` is the complete build — no Makefile wrapper, no external scripts mutating the config tree.

**Architecture:** Two Makefile preprocessing steps (`setup-host-configs`, `resolve-mpn`) become Tupfile-driven. Darwin configs use per-library `ifeq` selection. MPN source resolution becomes a config-generating rule. `Makefile.pup` is deleted; download logic moves to a standalone script.

**Tech Stack:** Tupfile rules, shell scripts, putup config-generating rules, CI YAML.

**Design doc:** `docs/plans/2026-03-09-eliminate-makefile-preprocessing-design.md`

---

### Task 1: Eliminate setup-host-configs — per-library darwin configs

The `setup-host-configs` Makefile target copies `configs/host-darwin/*.config` over each library's `defaults.config` when `HOST=darwin`. This mutates the config tree.

**Replacement:** Libraries with platform-specific differences get a `defaults-darwin.config` file. The Tupfile uses `ifeq (@(HOST_OS),darwin)` to select which config to copy. `@(HOST_OS)` comes from the root platform config (e.g., `darwin-x86_64-linux.config` has `CONFIG_HOST_OS=darwin`).

**Only 3 libraries need darwin variants** (the others have identical configs across platforms):
- binutils (HAVE_ALLOCA_H, DECL_BASENAME, DECL_SBRK, DECL_ENVIRON differ)
- gcc/gcc (MALLINFO, POSIX_FALLOCATE, GNU_AS/LD, asm features differ)
- gcc/libcpp (*_unlocked functions, DECL_BASENAME, SIZEOF_DEV_T differ)

For libbacktrace, the only darwin difference is `BACKTRACE_FORMAT=macho` — this goes in the root darwin platform config since no other library uses that key.

**Files:**
- Create: `examples/bsp/binutils/defaults-darwin.config`
- Create: `examples/bsp/gcc/gcc/defaults-darwin.config`
- Create: `examples/bsp/gcc/libcpp/defaults-darwin.config`
- Modify: `examples/bsp/binutils/Tupfile:1-2`
- Modify: `examples/bsp/gcc/gcc/Tupfile:1-2`
- Modify: `examples/bsp/gcc/libcpp/Tupfile:1-2`
- Modify: `examples/bsp/configs/darwin-x86_64-linux.config`
- Delete: `examples/bsp/configs/host-darwin/` (entire directory)

**Step 1: Create defaults-darwin.config for binutils**

Copy `configs/host-darwin/binutils.config` to `binutils/defaults-darwin.config`:

```bash
cp examples/bsp/configs/host-darwin/binutils.config examples/bsp/binutils/defaults-darwin.config
```

**Step 2: Create defaults-darwin.config for gcc**

```bash
cp examples/bsp/configs/host-darwin/gcc.config examples/bsp/gcc/gcc/defaults-darwin.config
```

**Step 3: Create defaults-darwin.config for libcpp**

```bash
cp examples/bsp/configs/host-darwin/libcpp.config examples/bsp/gcc/libcpp/defaults-darwin.config
```

**Step 4: Update binutils/Tupfile — ifeq darwin selection**

Replace line 2:
```tup
: defaults.config |> ^ INSTALL %o^ cp %f %o |> tup.config
```

With:
```tup
ifeq (@(HOST_OS),darwin)
: defaults-darwin.config |> ^ INSTALL %o^ cp %f %o |> tup.config
else
: defaults.config |> ^ INSTALL %o^ cp %f %o |> tup.config
endif
```

**Step 5: Update gcc/gcc/Tupfile — ifeq darwin selection**

Same pattern as step 4: replace line 2 with the ifeq block.

**Step 6: Update gcc/libcpp/Tupfile — ifeq darwin selection**

Same pattern as step 4: replace line 2 with the ifeq block.

**Step 7: Update darwin-x86_64-linux.config**

Add libbacktrace override and update comments. The file should become:

```
# GCC Libraries - macOS host, x86-64 Linux target (cross-compiler)
#
# Usage:
#   putup configure --config configs/darwin-x86_64-linux.config \
#     -C . -S ../../source-root -B ../../build-gcc
#
# Builds cc1 on macOS (Apple Silicon) that generates x86_64 Linux code.
# Host compiler is Apple Clang; target config reuses x86_64-pc-linux-gnu.

# Toolchain (Apple Clang as host compiler)
CONFIG_CC=clang
CONFIG_CXX=clang++
CONFIG_AR=ar
CONFIG_HOSTCC=clang++

# Platform selection
CONFIG_TARGET=x86_64-pc-linux-gnu
CONFIG_HOST_OS=darwin
CONFIG_PLATFORM_LDFLAGS=-lm -lpthread -lz -liconv
CONFIG_EXTRA_MODES_FILE=config/i386/i386-modes.def
CONFIG_DECIMAL_FORMAT=bid

# MPN CPU target (generic for cross-compile)
CONFIG_MPN_CPU=generic

# libbacktrace: Mach-O format on macOS
CONFIG_BACKTRACE_FORMAT=macho
```

**Step 8: Delete host-darwin/ directory**

```bash
rm -rf examples/bsp/configs/host-darwin/
```

**Step 9: Commit**

```bash
git add examples/bsp/binutils/defaults-darwin.config \
  examples/bsp/gcc/gcc/defaults-darwin.config \
  examples/bsp/gcc/libcpp/defaults-darwin.config \
  examples/bsp/binutils/Tupfile \
  examples/bsp/gcc/gcc/Tupfile \
  examples/bsp/gcc/libcpp/Tupfile \
  examples/bsp/configs/darwin-x86_64-linux.config
git rm -rf examples/bsp/configs/host-darwin/
git commit -m "bsp: eliminate setup-host-configs — use ifeq for darwin config selection

Libraries with darwin-specific configs (binutils, gcc, libcpp) get a
defaults-darwin.config alongside their defaults.config. The Tupfile
selects via ifeq (@(HOST_OS),darwin). No more Makefile preprocessing.

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>"
```

---

### Task 2: Make resolve-mpn a config-generating Tupfile rule

The `resolve-mpn` Makefile target runs `scripts/resolve-mpn.sh` as a preprocessing step to generate `gcc/gmp/mpn/defaults.config` and append MPN-dependent lines to `gcc/gmp/defaults.config`. This becomes Tupfile-driven.

**Key insight:** During `putup configure`, only the root `tup.config` is active for `@()` variable resolution. So `CONFIG_MPN_CPU` must be in the root platform config for the config-generating rules to read it.

**Files:**
- Modify: `examples/bsp/configs/x86_64-linux.config`
- Modify: `examples/bsp/gcc/gmp/defaults.config:100-101`
- Modify: `examples/bsp/gcc/gmp/Tupfile:2`
- Modify: `examples/bsp/gcc/gmp/mpn/Tupfile:1-2`
- Delete: `examples/bsp/gcc/gmp/mpn/defaults.config`

**Step 1: Add CONFIG_MPN_CPU to x86_64-linux.config**

Append after the `CONFIG_BACKTRACE_ELF_SIZE=64` line:

```
# MPN CPU target (generic = pure C, x86_64 = arch asm, x86_64/core2 = CPU asm)
CONFIG_MPN_CPU=generic
```

**Step 2: Remove MPN-dependent lines from gmp/defaults.config**

Remove the last 2 lines of `examples/bsp/gcc/gmp/defaults.config`:

```
CONFIG_GMP_MPARAM=mpn/generic/gmp-mparam.h
CONFIG_NO_ASM=1
```

These are now generated by the Tupfile config rule.

**Step 3: Update gmp/Tupfile config rule**

Replace line 2:
```tup
: defaults.config |> ^ INSTALL %o^ cp %f %o |> tup.config
```

With a rule that copies defaults.config and appends MPN-dependent variables:
```tup
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

**Why this works:** `@(MPN_CPU)` is expanded by putup at parse time from the root config. The shell sees a literal string (e.g., `cpu="generic"` or `cpu="x86_64/core2"`). The `${cpu%%/*}` pattern strips the CPU subdir to get the arch. CWD at execution time is source-tree `gcc/gmp/`, so `mpn/$arch/gmp-mparam.h` resolves correctly.

**Step 4: Update gmp/mpn/Tupfile config rule**

Replace line 2:
```tup
: defaults.config |> ^ INSTALL %o^ cp %f %o |> tup.config
```

With:
```tup
: $(S)/scripts/resolve-mpn.sh |> ^ RESOLVE-MPN %o^ sh %f @(MPN_CPU) . > %o |> tup.config
```

**How this works:**
- `$(S)` = BSP root (set in root `Tuprules.tup` as `$(TUP_CWD)`)
- `$(S)/scripts/resolve-mpn.sh` references the config tree's script file
- putup resolves the input to the correct cross-tree path (`%f` works via PR #11 fix)
- `sh %f` runs the script regardless of execute permissions
- `@(MPN_CPU)` is expanded at parse time from root config (e.g., `generic`)
- `.` is the mpn source directory — CWD at execution time is source-tree `gcc/gmp/mpn/`
- stdout redirects to `%o` which becomes `tup.config`

**Step 5: Delete gmp/mpn/defaults.config**

```bash
rm examples/bsp/gcc/gmp/mpn/defaults.config
```

**Step 6: Commit**

```bash
git add examples/bsp/configs/x86_64-linux.config \
  examples/bsp/gcc/gmp/defaults.config \
  examples/bsp/gcc/gmp/Tupfile \
  examples/bsp/gcc/gmp/mpn/Tupfile
git rm examples/bsp/gcc/gmp/mpn/defaults.config
git commit -m "bsp: make resolve-mpn a config-generating Tupfile rule

MPN source resolution (generic C vs arch-specific asm) is now driven
by @(MPN_CPU) in the platform config, read by Tupfile rules during
putup configure. No more Makefile preprocessing step.

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>"
```

---

### Task 3: Remove Makefile.pup — extract download script

With both preprocessing steps eliminated, `Makefile.pup` has no purpose. Extract the download logic into a standalone shell script.

**Files:**
- Create: `examples/bsp/scripts/download-source.sh`
- Delete: `examples/bsp/Makefile.pup`

**Step 1: Create download-source.sh**

```bash
#!/bin/sh
# download-source.sh — Download and assemble GCC + binutils source trees
#
# Usage: scripts/download-source.sh [SRCDIR]
#
# Environment:
#   GCC_VERSION      GCC version to download (default: 15.2.0)
#   BINUTILS_VERSION binutils version to download (default: 2.44)

set -e

GCC_VERSION=${GCC_VERSION:-15.2.0}
BINUTILS_VERSION=${BINUTILS_VERSION:-2.44}
SRCDIR=${1:-../../source-root}

# Download binutils
if [ ! -d "$SRCDIR/binutils" ]; then
    echo "Downloading binutils $BINUTILS_VERSION..."
    mkdir -p "$SRCDIR/binutils"
    curl -L "https://ftp.gnu.org/gnu/binutils/binutils-$BINUTILS_VERSION.tar.xz" \
        | tar xJ --strip-components=1 -C "$SRCDIR/binutils"
else
    echo "$SRCDIR/binutils already exists"
fi

# Download GCC + prerequisites
if [ ! -d "$SRCDIR/gcc" ]; then
    echo "Downloading GCC $GCC_VERSION..."
    mkdir -p "$SRCDIR/gcc"
    curl -L "https://ftp.gnu.org/gnu/gcc/gcc-$GCC_VERSION/gcc-$GCC_VERSION.tar.xz" \
        | tar xJ --strip-components=1 -C "$SRCDIR/gcc"
    cd "$SRCDIR/gcc" && ./contrib/download_prerequisites
else
    echo "$SRCDIR/gcc already exists"
fi
```

Make executable:
```bash
chmod +x examples/bsp/scripts/download-source.sh
```

**Step 2: Delete Makefile.pup**

```bash
rm examples/bsp/Makefile.pup
```

**Step 3: Commit**

```bash
git add examples/bsp/scripts/download-source.sh
git rm examples/bsp/Makefile.pup
git commit -m "bsp: remove Makefile.pup, extract download script

The canonical build command is now:
  putup configure --config configs/x86_64-linux.config -C . -S ../src -B ../build
  putup -C . -S ../src -B ../build -j\$(nproc)

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>"
```

---

### Task 4: Update CI workflow

Replace `make -f Makefile.pup` with direct putup invocations.

**Files:**
- Modify: `.github/workflows/ci.yml:173-196`

**Step 1: Update build-gcc-bsp job**

Replace the `Download source` and `Build` steps (lines 186-196):

```yaml
  build-gcc-bsp:
    needs: build-linux
    runs-on: ubuntu-latest
    timeout-minutes: 20
    steps:
      - uses: actions/checkout@v4
      - name: Download build artifacts
        uses: actions/download-artifact@v4
        with:
          name: build-linux
          path: build/
      - name: Fix permissions
        run: chmod +x build/putup
      - name: Download source
        working-directory: examples/bsp
        run: scripts/download-source.sh ../../source-root
      - name: Configure
        working-directory: examples/bsp
        run: ../../build/putup configure --config configs/x86_64-linux.config -C . -S ../../source-root -B ../../build-gcc
      - name: Build
        working-directory: examples/bsp
        run: ../../build/putup -C . -S ../../source-root -B ../../build-gcc -j$(nproc)
      - name: Smoke test cc1
        run: |
          echo 'int main() { return 0; }' > /tmp/test.c
          build-gcc/gcc/gcc/cc1 /tmp/test.c -quiet -o /tmp/test.s
          grep 'GCC.*15.2.0' /tmp/test.s
```

**Step 2: Commit**

```bash
git add .github/workflows/ci.yml
git commit -m "ci: use direct putup commands for BSP build

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>"
```

---

### Task 5: Update documentation and skill files

**Files:**
- Modify: `.claude/skills/gcc-example/SKILL.md:12-15`
- Modify: `docs/reference.md` (grep for Makefile.pup references)

**Step 1: Update SKILL.md build commands**

Replace lines 12-15 (the `make -f Makefile.pup` section):

```markdown
## Build Command

```bash
cd /path/to/pup/examples/bsp && \
  scripts/download-source.sh ../../source-root && \
  putup configure --config configs/x86_64-linux.config \
    -C . -S ../../source-root -B ../../build-gcc && \
  putup -C . -S ../../source-root -B ../../build-gcc -j$(nproc)
```

Or step by step:

```bash
cd /path/to/pup/examples/bsp && \
  putup configure --config configs/x86_64-linux.config \
    -C . -S ../../source-root -B ../../build-gcc
  putup -C . -S ../../source-root -B ../../build-gcc -j$(nproc)
```
```

Remove the "Or directly with putup:" paragraph (that IS the primary command now).

**Step 2: Update self-contained library convention section**

In SKILL.md, update:
```
- Its own `tup.config` for library-specific config
```
to:
```
- Its own `defaults.config` for library-specific config (copied to `tup.config` by Tupfile rule)
```

**Step 3: Search reference.md for Makefile.pup references**

Run: `grep -n Makefile.pup docs/reference.md`

Update or remove any references. The main one is likely in the examples section.

**Step 4: Commit**

```bash
git add .claude/skills/gcc-example/SKILL.md docs/reference.md
git commit -m "docs: update BSP build commands — Makefile.pup removed

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>"
```

---

### Task 6: Verify via CI

**Step 1: Push branch and create PR**

```bash
git push -u origin HEAD
gh pr create --title "bsp: eliminate Makefile.pup preprocessing" --body "..."
```

**Step 2: Monitor CI**

The `build-gcc-bsp` job is the critical gate. It will:
1. Download source tarballs via the new `download-source.sh`
2. Run `putup configure` (which executes config-generating rules including resolve-mpn)
3. Build the full GCC + binutils toolchain
4. Smoke test cc1

Expected: all green. If `build-gcc-bsp` fails, check:
- `resolve-mpn.sh` path resolution in 3-tree mode (`%f` expansion)
- `@(MPN_CPU)` availability during configure (must be in root config)
- ifeq `@(HOST_OS)` resolution (must be in root config — linux config doesn't set it, so the `else` branch fires, which is correct)

**Important:** `x86_64-linux.config` does NOT set `CONFIG_HOST_OS`. This means `@(HOST_OS)` is empty during configure for linux builds. The `ifeq (@(HOST_OS),darwin)` will NOT match, so the `else` branch (linux defaults.config) is used. This is the correct behavior.
