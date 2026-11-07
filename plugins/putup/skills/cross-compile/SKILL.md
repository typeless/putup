---
name: cross-compile
description: Cross-compilation and BSP development with putup. Use when setting up cross-compile toolchains, 3-tree builds, generator programs, config headers, multi-library BSP projects, or Windows (clang-cl + xwin) targets.
---

# Cross-Compilation with Putup

Full reference: <https://github.com/typeless/putup/blob/main/docs/reference.md>

## Three-Tree Architecture

Separate source, config, and build into independent trees:

```bash
putup -S source_dir -C config_dir -B build_dir
```

| Flag | Tree    | Description                                       |
|------|---------|---------------------------------------------------|
| `-S` | Source  | Read-only source code (upstream, git submodules)   |
| `-C` | Config  | Tupfiles, Tuprules.tup, platform `*.config` files |
| `-B` | Build   | Outputs, .pup/ index, root tup.config              |

Commands run with CWD = source directory. The source tree is read-only.

The root `tup.config` is read from the build tree, where `putup configure --config FILE` installs it; a `tup.config` in the config tree is ignored. Every directory holding a Tupfile must also exist in the source tree: a rule in a config-only directory fails because its CWD is missing.

putup refuses the Tupfiles tup refuses; see "What putup refuses" in the tupfile-authoring skill.

### When to use each mode

| Mode         | Flags       | Use case                                      |
|--------------|-------------|-----------------------------------------------|
| In-tree      | (none)      | Simple single-config projects                 |
| Variant      | `-B`        | Multiple configs (debug/release) same source  |
| Two-tree     | `-S -B`     | Out-of-tree build, Tupfiles live with source  |
| Three-tree   | `-S -C -B`  | BSP: build upstream code without modifying it |

### Key Variables

| Variable                    | Meaning                                            |
|-----------------------------|----------------------------------------------------|
| `$(TUP_CWD)`               | Config-relative directory of current Tupfile        |
| `$(TUP_VARIANTDIR)`        | Like `$(TUP_CWD)`, but pointing at the corresponding directory in the variant tree (e.g. `../../build-arm/src/lib`) |
| `$(TUP_VARIANT_OUTPUTDIR)` | Relative path from the Tupfile's source dir to its output dir in the variant tree (e.g. `../../build/src/lib`; `.` in-tree) |
| `$(S)` (convention)        | Set to `$(TUP_CWD)` in root Tuprules.tup           |
| `$(B)` (convention)        | Set to `$(TUP_VARIANT_OUTPUTDIR)/$(S)`             |
| `$(TUP_SRCDIR)`            | Relative path from Tupfile dir to source dir        |
| `$(TUP_OUTDIR)`            | Relative path from Tupfile dir to output dir        |

Climb from `$(TUP_VARIANTDIR)` to anchor other directories' variant outputs:
from an aggregation Tupfile two levels deep, `$(TUP_VARIANTDIR)/../..` is the
variant root, so `mods-y += $(TUP_VARIANTDIR)/../../modules/kernel/kernel.hex`
references another directory's generated file.

## CROSS_COMPILE Convention

Follow the Linux kernel pattern. Import the prefix and derive tools:

```tup
# Tuprules.tup
import CROSS_COMPILE=

import CC=$(CROSS_COMPILE)gcc
import CXX=$(CROSS_COMPILE)g++
import AR=$(CROSS_COMPILE)ar
import LD=$(CROSS_COMPILE)ld
import STRIP=$(CROSS_COMPILE)strip
import OBJCOPY=$(CROSS_COMPILE)objcopy
```

Build with the prefix set in the environment:

```bash
# Native build (CROSS_COMPILE defaults to empty)
putup -B build

# ARM cross-compile
CROSS_COMPILE=arm-none-eabi- putup -B build-arm

# Override a single tool
CROSS_COMPILE=arm-none-eabi- CC=clang putup -B build-arm-clang
```

## MSVC-ABI targets (clang-cl + xwin)

The `CROSS_COMPILE`-prefix pattern is for GNU-style toolchains. Windows (MSVC ABI) targets use a different trio and flag style — see `configs/xwin.config` in the putup tree as the worked reference:

- Compiler `clang-cl`, linker `lld-link`, archiver `llvm-lib` (not `gcc`/`ld`/`ar`).
- `--target=x86_64-pc-windows-msvc` to select the MSVC ABI.
- The MSVC CRT and Windows SDK come from an xwin-splatted tree injected via the `XWIN_SPLAT` environment variable.
- `/MT` links the static CRT.
- Flag syntax differs from GNU: `clang-cl` uses `/Fo` for object output and `/OUT:` for the link target (no `-o`), and reaches the dep-file flags through `/clang:-MD /clang:-MF%o.d` — so the `CROSS_COMPILE` bang-macro shapes above do not carry over.

CI runs the resulting test binary natively on a `windows-latest` runner; Wine runs it locally when no Windows host is at hand.

## Generator Programs

Generators produce source or header files during the build. In 3-tree mode, CWD is the read-only source dir, so generators that write files need special handling.

### Stdout redirect (single output)

Redirect generator stdout to the output file:

```tup
: input.md | generator |> ^ GEN %o^ \
  $(TUP_VARIANT_OUTPUTDIR)/generator %f > %o |> output.h
```

### cd to build dir (multiple file outputs)

Generators that write files via flags (`-O`, `-H`, `-c`, etc.) write relative to CWD. Change to the build dir first:

```tup
: $(MD) | generator |> ^ GEN outputs^ \
  cd $(TUP_VARIANT_OUTPUTDIR) && ./generator %f \
  -O output-1.cc -O output-2.cc \
  |> output-1.cc output-2.cc
```

When `%f` paths break after cd (they were source-relative), capture CWD first:

```tup
: $(MD) | generator |> ^ GEN outputs^ \
  SRCDIR=$PWD && cd $(TUP_VARIANT_OUTPUTDIR) && \
  ./generator $SRCDIR/input1.md $SRCDIR/input2.md \
  -O output-1.cc -O output-2.cc \
  |> output-1.cc output-2.cc
```

`$SRCDIR` and `$PWD` are shell variables. Bare `$` passes through putup to the shell; only `$(VAR)` is expanded by putup.

## Host Tools

When cross-compiling, you need host-native tools (code generators, format
converters). Don't use `!cc` — that's the cross-compiler. Use the host
compiler directly, or define a `!host_cc` macro:

```tup
import HOSTCC=gcc
!host_cc = |> ^ HOSTCC %b^ $(HOSTCC) -Wall -O2 -o %o %f |>
```

```tup
# tools/Tupfile — built for the HOST, not the target
: hex2bin.c \
|> !host_cc \
|> hex2bin

# Use host tool in a generator rule
: input.dat | tools/hex2bin \
|> $(TUP_VARIANT_OUTPUTDIR)/tools/hex2bin %f > %o \
|> generated_table.h <gen-headers>
```

The tool is an output, so it lives in the build tree; a bare `tools/hex2bin` resolves against the source directory and fails under `-B`.

## Config Headers

### tup.config to C #defines (`!gen-config`)

Convert `CONFIG_*` variables into a C header:

```tup
!gen-config = |> ^ GEN %o^ awk -F= \
  '/^CONFIG_/{k=substr($1,8);v=substr($0,length($1)+2); \
  if(v!="n")print "#define " k " " (v=="y"?1:v)}' \
  %f > %o |>
```

`CONFIG_HAVE_MMAP=1` becomes `#define HAVE_MMAP 1`. Setting `CONFIG_HAVE_UCHAR=n` suppresses the define.

Declare the config as the rule's input so the header rebuilds when it changes
(`$(B)/tup.config` and a root-relative `../../tup.config` both resolve to the
variant tree's copy):

```tup
: $(B)/tup.config |> !gen-config |> config.h <gen-headers>
```

### Separate host vs target headers

BSP projects need distinct headers for generators (host) vs compiled objects (target):

```tup
# bconfig.h - for generators running on the build machine
# config.h  - for target objects, errors if GENERATOR_FILE defined
```

Generators compile with `-DGENERATOR_FILE` to gate which headers they see. Target objects omit this flag.

## BSP Workflow

1. **Download sources** into the source tree:
   ```bash
   scripts/download-source.sh ../../source-root
   ```

2. **Configure** with platform-specific config:
   ```bash
   putup configure --config configs/x86_64-linux.config \
     -C . -S ../../source-root -B ../../build-gcc
   ```
   `examples/bsp/configs/` also has `darwin-x86_64-linux.config` (macOS host) and `aarch64-linux.config`.

3. **Build** with parallelism:
   ```bash
   putup -C . -S ../../source-root -B ../../build-gcc -j$(nproc)
   ```

4. **Debug failures** by reading the first error, ignoring cascading failures. Check group deps for missing headers, check generator patterns for read-only filesystem errors.

5. **Iterate** -- after editing a component's `defaults.config`, re-run `putup configure` so its copy rule reinstalls `tup.config` in the build dir; a plain build does not. After editing the platform config, re-run `putup configure --config FILE`; without `--config` the installed copy stays unchanged. Edits made directly to the build tree's `tup.config` apply on the next build.

The GCC + binutils BSP in `examples/bsp/` is the worked example: see [references/gcc-bsp-example.md](references/gcc-bsp-example.md).

## Common Errors

| Error | Cause | Fix |
|-------|-------|-----|
| `Read-only file system` | Generator writes to CWD (source dir) | cd to build dir before running |
| `No such file or directory` (output) | Same as above | cd to build dir |
| `undeclared` from generated header | Missing order-only group dependency | Add `\| <gen-headers>` (another directory's group: `\| $(S)/dir/<gen-headers>`) to the rule |
| Config define missing at compile time | Edited config not installed in the build tree | Re-run `putup configure` (with `--config FILE` for the platform config) |
| `FAILED:` with no error output, in a config-only directory | CWD is the source directory, which does not exist | Keep each Tupfile's directory present in the source tree |
| Generator reads wrong input after cd | `%f` paths are source-relative | Capture `SRCDIR=$PWD` before cd |
| Circular dependency on generated header | Generator depends on its own output group | Split into separate generation phases |
