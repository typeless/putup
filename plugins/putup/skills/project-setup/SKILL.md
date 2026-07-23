---
name: project-setup
description: Setting up putup projects. Use when installing putup, initializing a project, configuring variant builds, choosing build modes, running putup in CI, running diagnostics, or using putup commands.
---

# Project Setup

Guide for initializing and configuring putup projects. For the full manual, see
<https://github.com/typeless/putup/blob/main/docs/reference.md>.

## Installing putup

putup ships prebuilt binaries. Install the latest release with one command:

```bash
curl -fsSL https://raw.githubusercontent.com/typeless/putup/main/install.sh | sh
```

This downloads a prebuilt binary to `~/.local/bin` (plus a `pup` alias). It needs
`curl` or `wget` — no compiler. Overrides:

- `PUTUP_VERSION=vX.Y.Z` — pin a specific release instead of the latest (also
  skips the GitHub API lookup). Pick a tag from the
  [releases page](https://github.com/typeless/putup/releases).
- `PUTUP_INSTALL_DIR=/usr/local/bin` — install elsewhere (default `~/.local/bin`).

Prebuilt binaries are published for **Linux x86_64**, **macOS arm64**, and
**Windows x86_64**. On any other platform (e.g. Linux arm64, Intel macOS) the
script stops — build from source instead (needs a C++20 compiler: GCC 11+ or
Clang 14+). See [INSTALL.md](https://github.com/typeless/putup/blob/main/INSTALL.md).

### Using putup in CI

Building a putup project in CI is plain shell — install the binary, put it on
`PATH`, then configure and build. These steps drop into any CI system (a GitHub
Actions `run:` block, a GitLab `script:` step, CircleCI, Jenkins, a Makefile
target, …):

```sh
# 1. Install putup (prebuilt binary; needs curl or wget, no compiler).
#    Pin PUTUP_VERSION to a release tag so CI is reproducible (also skips the
#    GitHub API lookup). Tags: https://github.com/typeless/putup/releases
export PUTUP_VERSION=vX.Y.Z
curl -fsSL https://raw.githubusercontent.com/typeless/putup/main/install.sh | sh

# 2. Put it on PATH (install.sh installs to ~/.local/bin by default).
export PATH="$HOME/.local/bin:$PATH"

# 3. Build the project from its root (where Tupfile.ini lives).
putup --version      # sanity check
putup configure      # create tup.config (config step; once per checkout)
putup                # build
```

- **Variant / out-of-tree builds:** use `putup configure -B build && putup -B build`
  (see Build Modes).
- **No prebuilt binary** for the runner (e.g. Linux arm64): install a C++20
  compiler and build from source per INSTALL.md, or vendor a binary you built.
- **Offline / locked-down runners:** cache `~/.local/bin` between runs, or commit a
  pinned binary, to avoid fetching from GitHub on every build.

## 1. Quick Start

```bash
mkdir hello && cd hello
touch Tupfile.ini
echo ': main.c |> gcc %f -o %o |> hello' > Tupfile
echo 'int main() { return 0; }' > main.c

putup configure          # creates tup.config
putup                    # build
./hello                  # run
```

Required files:

| File | Purpose |
|------|---------|
| `Tupfile.ini` | Marks project root (can be empty). A subdirectory with its own `Tupfile.ini` is a separate project — pruned from this build unless a rule references a group/file under it |
| `Tupfile` | Build rules |
| `tup.config` | Config variables (created by `putup configure`) |

## 2. Commands

| Command | What it does |
|---------|-------------|
| `putup` | Build (default command) |
| `putup parse` | Parse and validate Tupfiles without building |
| `putup clean` | Remove generated output files |
| `putup distclean` | Full reset: remove outputs, `.pup/`, and `tup.config` |
| `putup configure` | Run config-generating rules; create `tup.config` |
| `putup show <fmt>` | Show build info (`script`, `compdb`, `graph`, `var`, `instructions`) |

**Key flags:**

| Flag | Effect |
|------|--------|
| `-j N` | Parallel jobs (default: CPU count) |
| `-k` | Continue after failures |
| `-n` | Dry run (print commands, don't execute) |
| `-v` | Verbose output |
| `-A` | Build everything (ignore CWD scoping) |
| `-a` | Include upstream deps in scoped builds |
| `-D VAR=val` | Override a CONFIG_ variable |
| `-B DIR` | Build directory (can repeat for multiple variants) |
| `-S DIR` | Source directory |
| `-C DIR` | Config directory (where Tupfiles live) |
| `-c FILE` | Install FILE as root tup.config (configure only) |
| `--stat` | Print build statistics |

## 3. Build Modes

### In-Tree (simplest)

Outputs sit alongside sources. Good for small projects:

```bash
putup configure && putup
```

### Variant Build (`-B`)

Separate output directory. Multiple variants can coexist:

```bash
putup configure -B build-debug
putup configure -B build-release
putup build-debug build-release    # build both in parallel
```

Result:
```
project/
  Tupfile.ini
  Tupfile
  main.c
  build-debug/                     # variant directory
    tup.config
    main.o
    .pup/
```

### Three-Tree Build (`-C/-S/-B`)

Source, Tupfiles, and output in three separate trees. Build third-party code
without modifying it:

```bash
putup -S vendor/busybox -C config -B build
```

| Tree | Flag | Contains |
|------|------|----------|
| Source | `-S` | Upstream source (read-only) |
| Config | `-C` | Your Tupfiles and Tuprules.tup |
| Output | `-B` | Build outputs and `.pup/` |

## 4. Variant Builds in Depth

**Create with a pre-made config:**

```bash
putup configure -B build-arm --config configs/arm-cross.config
```

**Auto-detection:** Without `-B`, putup discovers all subdirectories
containing `tup.config` or `.pup/` and builds them in parallel:

```bash
putup                              # builds every discovered variant
```

**Select by path or glob:**

```bash
putup build-debug                  # single variant
putup build-*                      # glob -- all matching variants
putup build-debug build-release    # explicit list
putup -B build-debug -B build-release   # explicit -B flags
```

**Clean a variant:**

```bash
putup clean build-debug            # remove outputs only
putup distclean build-debug        # remove entire variant directory
```

**Adopting an existing tup project:** putup builds unmodified tup projects.
The `tup variant` workflow maps directly:

```bash
# tup
tup variant configs/board.config && tup

# putup equivalent
putup configure --config configs/board.config -B build-board
putup -B build-board
```

- Kconfig-generated configs work verbatim as tup.config (`# CONFIG_X is not
  set` lines are comments).
- `configure` printing `No config-generating rules found.` is informational --
  it just means the project has no two-stage config rules.
- Start with `putup parse -B build-board` to validate all Tupfiles before the
  first build.
- The variant directory name is embedded in outputs by `-g` (debug info paths)
  and by tools that print `%f` (e.g. `objdump`), so keep the name stable if
  you compare build trees byte-for-byte.

## 5. Scoped Builds

Run from a subdirectory to build only that subtree:

```bash
cd project/lib && putup            # builds only lib/ outputs
putup lib app                      # explicit: build lib/ and app/ only
putup -A                           # full project, ignore CWD scope
```

Combine scopes with variants:

```bash
putup build-debug/src/lib          # variant + directory scope
putup build-*/src/lib              # all variants, scoped to lib/
```

Include upstream dependencies with `-a`:

```bash
putup lib                          # fast: only check files in lib/
putup -a lib                       # also check lib/'s upstream deps
```

Exclude directories with `-x` (gitignore-style pattern, repeatable):

```bash
putup -x tests/                    # build all, skip every tests/ dir
putup src -x 'src/tests/'          # composes with scoping
```

Exclusion is per-invocation and schedule-level: excluded directories keep
their index state, so toggling `-x` never causes spurious rebuilds; their
changes are deferred until a build without `-x`.

## 6. Configuration

### tup.config Format

```ini
CONFIG_CC=clang
CONFIG_CFLAGS=-Wall -O2
CONFIG_DEBUG=y
```

Names must start with `CONFIG_`. Access in Tupfiles with `@(NAME)`:

```tup
CC = @(CC)                         # resolves to "clang"
DEBUG = @(DEBUG:-n)                # default value if unset
```

### CLI Overrides (`-D`)

Highest precedence, overrides tup.config values:

```bash
putup -D CC=clang -D DEBUG=n
putup -DDEBUG                      # shorthand for -D DEBUG=y
```

### Scoped Config

Subdirectories can have their own `tup.config`. Parent values override
child values on collision (integrator wins):

```
build/tup.config           # CONFIG_CC=clang  (highest precedence)
build/gmp/tup.config       # CONFIG_CC=gcc    (overridden by parent)
```

Ship component defaults with a copy rule in the Tupfile:

```tup
: defaults.config |> cp %f %o |> tup.config
```

**Precedence:** `-D` overrides > root tup.config > intermediate > leaf configs.

## 7. Diagnostics

```bash
putup --stat                              # build statistics
putup show compdb > compile_commands.json # compilation database (for clangd)
putup show graph --summary                # text dependency overview
putup show graph | dot -Tpng -o deps.png  # graphviz visualization
putup show graph --all-deps               # include implicit header deps
putup show var CC                         # variable assignment history
putup show var --json                     # machine-readable variable dump
putup parse --check=error                 # check composability conventions (alias: --strict)
putup show instructions                   # command deduplication analysis
putup show index --summary                # on-disk index counts (forensic)
putup show index PATTERN                  # per-command implicit/sticky deps
```

Plain `parse` reports whether component `Tuprules.tup` files use `?=` for
anchor variables (`S`, `B`) and toolchain variables (`CC`, `CXX`, `AR`);
`--check=error` (alias `--strict`) makes those violations fail the exit code.

## 8. Troubleshooting

| Error | Cause | Fix |
|-------|-------|-----|
| `Not in a putup project` | No `Tupfile.ini` found | Create `Tupfile.ini` at project root |
| `No tup.config found` | Skipped configure step | Run `putup configure` |
| Build outputs in wrong directory | In-tree when variant intended | Use `-B build` |
| `Tupfile.ini not found` with `-B` | Source root missing marker | Add `Tupfile.ini` to source root |
| Stale outputs after rule change | Normal behavior | putup auto-cleans; `putup clean` for full reset |
| Variant not discovered | Missing `tup.config` or `.pup/` | Run `putup configure -B <dir>` |

**Standard workflow:**

```bash
putup configure -B build       # 1. generate tup.config
putup -B build                 # 2. build
```

**Multi-variant workflow:**

```bash
putup configure -B build-debug --config configs/debug.config
putup configure -B build-release --config configs/release.config
putup                          # auto-detects and builds both
```

For the full reference, see <https://github.com/typeless/putup/blob/main/docs/reference.md>.
