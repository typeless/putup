# Putup Reference Manual

## 1. Introduction

### 1.1 What is Putup?

Putup is a build system that uses [Tup's](https://gittup.org/tup/) Tupfile syntax. It provides fast incremental builds through content-based change detection (SHA-256 hashing) and a dependency graph that tracks exactly what needs to rebuild.

Key characteristics:
- **Compatible with Tup** - Parses existing Tupfiles without modification
- **Content hashing** - SHA-256 for precise change detection beyond timestamps
- **No FUSE** - Works anywhere without filesystem dependencies
- **Binary index** - Fast, portable build state (no SQLite)
- **Parallel execution** - Automatic parallelism within dependency constraints

### 1.2 Compatibility with Tup

Putup supports the core Tupfile syntax and most tup commands. It has no FUSE sandbox, no Lua and no
`run` directive, and it keeps its build record in a binary index instead of a SQLite database.
[COMPATIBILITY.md](../COMPATIBILITY.md) lists every command and syntax feature with its status, the
behavioural differences, and putup's extensions.

## 2. Quick Start

### 2.1 Installation

See [INSTALL.md](../INSTALL.md) for the install script, building from source, and bootstrapping
without an existing `putup`.

### 2.2 Your First Build

Create a simple project:

```bash
mkdir hello && cd hello
echo 'int main() { return 0; }' > main.c
echo ': main.c |> gcc %f -o %o |> hello' > Tupfile
touch Tupfile.ini

putup configure    # Initialize (creates tup.config)
putup              # Build
./hello            # Run
```

### 2.3 Project Structure

A minimal putup project:

```
project/
├── Tupfile.ini     # Project root marker (can be empty)
├── Tupfile         # Build rules
├── tup.config      # Configuration variables (created by configure)
└── .pup/           # Build state index (created on first build)
```

For variant (out-of-tree) builds:

```
project/
├── Tupfile.ini
├── Tupfile
├── src/
└── build/          # Variant directory
    ├── tup.config  # Variant-specific config
    ├── .pup/       # Variant's index
    └── hello       # Build output
```

## 3. Command Reference

### 3.1 putup (build)

```
putup [OPTIONS] [TARGETS...]
```

The default command. Executes the build by parsing Tupfiles, computing the dependency graph, and running commands for changed files.

**Build directory selection is always explicit.** Without `-B` flags or variant targets, putup uses only unambiguous context: running from inside a build directory (any depth) selects it, and a `tup.config` at the source root means an in-tree build. Putup never adopts a build directory by scanning — if candidates exist (subdirectories with `tup.config` or `.pup/`) and none was specified, it errors and lists them as hints. This makes adopting a build directory that belongs to another project or configuration impossible.

**Arguments:**
- `TARGETS` - Optional paths to scope the build. Can be:
  - **Variant directory:** `build-debug` (selects variant)
  - **Scope directory:** `src/lib` (limits to subtree)
  - **Combined:** `build-debug/src/lib` (variant + scope)
  - **Glob pattern:** `build-*` (multiple variants)
  - **Output file:** `build-debug/src/lib/foo.o` (single file rebuild)

**Relevant Options:**
- `-j N` - Run N jobs in parallel (default: number of CPUs)
- `-k` - Continue building after failures
- `-n` - Dry-run: print commands without executing
- `-v` - Verbose output (show parsed files, change detection, objects with no dependency scan)
- `-B DIR` - Build directory (can specify multiple times for parallel builds)
- `-A` - Build all files (ignore cwd scoping)
- `--stat` - Print build statistics after completion

**Progress Display:**

During builds, putup shows ninja-style progress output on TTY terminals:

```
[ 75% 45/60] src/parser.o
    1:23 src/graph/builder.o
    0:45 src/exec/runner.o
    0:12 src/cli/cmd_build.o
```

- Progress line shows percentage, completed/total, and current target
- Running jobs listed below, sorted by elapsed time (longest first)
- Time format: `M:SS` (minutes:seconds)
- Display updates in-place using terminal control sequences
- Each finished command is named on its own line above the display, by its `^ text ^` annotation when it has one; any output it wrote follows that line, each line carrying the same `[variant] ` prefix

When stdout is not a terminal — a pipe, a file, a CI log — the in-place display is off and each finished command's line carries the percent complete:

```
[.] ~33% CC src/parser.c
[.] ~66% CC src/graph/builder.c
[.] 100% LINK putup
[.] Build completed: 3 commands in 2104ms
```

`-q` names only the commands that fail or write output; `-v` names each command by its full command line instead of its annotation.

**Examples:**
```bash
putup                    # In-tree build, or the enclosing build dir when run
                         # from inside one; otherwise errors listing candidates
putup -j8                # Build with 8 parallel jobs
putup -v                 # Verbose build
putup lib app            # Build only lib/ and app/ directories
putup -n                 # Show what would be built

# Path-based variant selection
putup build-debug                  # Build single variant (path-based)
putup build-*                      # Build all variants matching pattern
putup build-debug build-release    # Build specific variants
putup build-debug/src/lib          # Variant + scoped build
putup src/lib                      # Scope applied to the selected build directory
putup build-debug/src/lib/foo.o    # Rebuild single output file

# Explicit -B flag (requires prior configure -B)
putup -B build-debug               # Build specific variant
putup -B build-debug -B build-release  # Multiple -B flags
```

### 3.2 putup parse

```
putup parse [OPTIONS] [TARGETS...]
```

Parse and validate all Tupfiles without executing any commands. Useful for checking syntax errors or seeing what would be built.
Supports path-based variant and scope selection.

**Relevant Options:**
- `-v` - Show each Tupfile as it's parsed
- `-S DIR` - Specify source directory
- `-B DIR` - Specify build directory (can use multiple times)

**Multi-Variant Support:**
- Running from project root requires an explicit variant (path target or -B); candidates are listed as hints
- Path-based targets: `putup parse build-debug`, `putup parse build-*`
- `-B DIR` also selects a build directory explicitly

**Convention checking (`--check`)**

`parse` verifies that component libraries (directories with their own
`Tuprules.tup`, reached via `include_rules`) follow the dual-mode composability
conventions that let a project build both standalone and as part of a larger
project:

- **Error:** Anchor variables `S` and `B` must use `?=` (not `=`) in component `Tuprules.tup`
- **Warning:** Toolchain variables (`CC`, `CXX`, `AR`, etc.) should use `?=`
- **Warning:** Component directories should contain `Tupfile.ini` for standalone builds

The tree roots — the source root and, in 3-tree builds, the config-tree root —
are exempt: they *are* the authority for anchor and toolchain variables.

The `--check=LEVEL` option selects how violations are handled:

| Level | Behavior |
|-------|----------|
| `none`  | Skip checks entirely. |
| `warn`  | Report violations to stderr, exit 0. **(default)** |
| `error` | Report violations to stderr, exit non-zero if any error. |

`--strict` is an alias for `--check=error` (use it in CI to fail on violations).

**Examples:**
```bash
putup parse                  # Validate all Tupfiles; report violations, exit 0
putup parse -v               # Show parsing progress
putup parse build-debug      # Parse single variant (path-based)
putup parse build-*          # Parse all matching variants
putup parse build-debug/lib  # Parse scoped to lib/ directory
putup parse --check=error    # Fail (exit non-zero) on convention violations
putup parse --strict         # Same as --check=error
putup parse --check=none     # Skip convention checks
```

### 3.3 putup clean

```
putup clean [OPTIONS] [TARGETS...]
```

Remove generated output files tracked in the index. Does not remove `.pup/` or `tup.config`.
Supports path-based variant and scope selection.

**Relevant Options:**
- `-n` - Dry-run: show what would be removed
- `-v` - Verbose: list each file removed
- `-B DIR` - Clean a variant build directory (can use multiple times)

**Multi-Variant Support:**
- Running from project root requires an explicit variant (path target or -B); candidates are listed as hints
- Path-based targets: `putup clean build-debug`, `putup clean build-*`
- `-B DIR` also selects a build directory explicitly

**Examples:**
```bash
putup clean                     # In-tree / from inside a build dir; else errors
putup clean -n                  # Show what would be removed
putup clean build-debug         # Clean single variant (path-based)
putup clean build-*             # Clean all matching variants
putup clean build-debug/src/lib # Clean scoped to src/lib
```

### 3.4 putup distclean

```
putup distclean [OPTIONS] [TARGETS...]
```

Full reset: remove all generated files, the `.pup/` directory, and `tup.config`. Returns the project to a pristine state. A build record this putup cannot read is kept, because it is the only description of what the build owned and another putup may still read it — remove it with `rm -rf` once you are sure.
Supports path-based variant selection.

**Relevant Options:**
- `-n` - Dry-run: show what would be removed
- `-B DIR` - Distclean a variant build directory (can use multiple times)

**Multi-Variant Support:**
- Running from project root requires an explicit variant (path target or -B); candidates are listed as hints
- Path-based targets: `putup distclean build-debug`, `putup distclean build-*`
- `-B DIR` also selects a build directory explicitly

**Examples:**
```bash
putup distclean             # In-tree / from inside a build dir; else errors
putup distclean build-debug # Reset single variant (path-based)
putup distclean build-*     # Reset all matching variants
```

### 3.5 putup configure

```
putup configure [OPTIONS] [TARGETS...]
```

Execute only rules that output `tup.config` files. Used for two-pass config deployment workflows where config files are generated before the main build.

**Use case:** Multi-project builds where subprojects need per-directory `tup.config` files generated from a central configuration.

**Workflow:**
```bash
putup configure    # Pass 1: Generate tup.config files
putup              # Pass 2: Build with generated configs
```

**How it works:**
1. Installs root config if `--config` is specified
2. Parses all Tupfiles using root `tup.config` only
3. Identifies rules where any output ends with `tup.config`
4. Executes only those rules (plus their dependencies)
5. Does not write to `.pup/index` (avoids conflict with subsequent build)

**Relevant Options:**
- `-v` - Verbose output
- `-k` - Continue after failures
- `-n` - Dry-run: show what would execute
- `-C DIR` - Config directory (where Tupfiles and tup.config live)
- `-S DIR` - Source directory (read-only source tree)
- `-B DIR` - Specify build directory (created automatically if it doesn't exist)
- `-c, --config FILE` - Install FILE as root tup.config before running config rules

**Note:** The `-B` flag creates the output directory if needed. After configure runs, the directory contains `tup.config` which marks it as a variant for subsequent builds. If no config-generating rules exist, an empty `tup.config` is created automatically. The `.pup/` index is NOT created during configure (it's created on first build).

**Using --config for pre-made configs:**

The `--config` option copies an existing config file to the output directory as the root `tup.config`. It then continues with steps 2–5 above, running any config-generating rules. Useful for:
- Cross-compilation with pre-made toolchain configs
- CI/CD where configs are externally managed
- Mixed workflows with a static root config + auto-generated subdir configs

```bash
putup configure -B build --config configs/arm-cross.config
putup configure -B build-debug -c debug.config
```

**Subdir config installation:**

Subdirectory configs are installed via Tupfile copy rules — putup has no built-in config copying. Each subdirectory that needs a scoped `tup.config` ships a `defaults.config` alongside its Tupfile with a copy rule:

```tup
: defaults.config |> cp %f %o |> tup.config
```

At configure time, these rules are discovered and executed like any other config-generating rule. The resulting `tup.config` files are placed in the build tree where scoped config merging (§6.1) picks them up during the build.

```bash
# Each subdir Tupfile has a copy rule for its defaults.config:
putup configure --config configs/toolchain.config -C . -S ../src -B ../build
#   → ../build/tup.config          (from --config)
#   → ../build/gmp/tup.config      (from gmp/Tupfile copy rule)
#   → ../build/mpfr/tup.config     (from mpfr/Tupfile copy rule)
#   → ../build/mpc/tup.config      (from mpc/Tupfile copy rule)
```

**Important:** You must run `putup configure` before `putup build`. If you skip the configure step, `putup build` will error:

```
Error: No tup.config found. Run 'pup configure' first.
```

This ensures a consistent workflow: configure sets up the build environment, then build executes.

**Example: Putup's own build**

Putup uses `putup configure` for its own build. The `configs/Tupfile` generates `build/tup.config`:

```tup
# configs/Tupfile - Generate tup.config for the variant build
import CONFIG=@(TUP_PLATFORM)
: $(CONFIG).config |> cp %f %o |> ../tup.config
```

`import CONFIG=@(TUP_PLATFORM)` imports the `CONFIG` environment variable, defaulting to the platform name (`linux`, `macosx`) when unset. The copy rule then installs `configs/$(CONFIG).config` as `build/tup.config`.

Build workflow:
```bash
putup configure -B build   # Generate build/tup.config from configs/linux.config (or macosx.config)
putup -B build             # Build with generated config
```

**Selecting a config with `CONFIG=<name>`:** Set the `CONFIG` environment variable to pick any `configs/<name>.config`:
```bash
CONFIG=coverage putup configure -B build-coverage   # configs/coverage.config
CONFIG=xwin putup configure -B build-win            # configs/xwin.config (Windows cross-build)
```

**Example: Multi-project with per-directory configs**

For projects where subprojects need their own `tup.config` files:

```
project/
├── configs/
│   ├── Tupfile           # Rules that output tup.config files
│   └── board-xyz.tup     # Config layer included by Tupfile
├── linux/
│   └── Tupfile           # Uses @(DEFCONFIG) from linux/tup.config
├── build-xyz/
│   ├── tup.config        # Root config: CONFIG_MACHINE=board-xyz
│   └── linux/
│       └── tup.config    # Generated by configs/Tupfile
```

```tup
# configs/Tupfile
include_rules
include machine/@(MACHINE).tup

# Copy pre-configured defconfig
: defconfigs/$(MACHINE)/linux.config |> install -D %f %o |> ../linux/tup.config
```

### 3.6 putup show

```
putup show <format> [OPTIONS] [TARGETS...]
```

Show build information in various formats. Supports path-based variant and scope selection.

**Formats:**
- `script` - Shell script
- `compdb` - compile_commands.json
- `graph` - DOT format dependency graph
- `var` - Variable assignment history
- `instructions` - Command instruction deduplication analysis
- `index` - On-disk index dump (forensic / debugging)

**Examples with targets:**
```bash
putup show graph --summary build-debug    # Single variant
putup show compdb build-*                 # All matching variants
putup show graph build-debug/src/lib      # Variant + scope
```

#### 3.6.1 show script

```
putup show script > build.sh
```

Generate a shell script that runs all build commands in topological order. Useful for environments where putup isn't available or for debugging.

**Output:** Shell script to stdout

#### 3.6.2 show compdb

```
putup show compdb > compile_commands.json
```

Generate a [compilation database](https://clang.llvm.org/docs/JSONCompilationDatabase.html) for IDE integration. Works with clangd, ccls, and other tools.

**Output:** JSON array of compilation commands

**Example usage with clangd:**
```bash
putup show compdb > compile_commands.json
# IDE now has full code intelligence
```

#### 3.6.3 show graph

```
putup show graph [OPTIONS]
```

Show the dependency graph for visualization or analysis.

**Options:**
- `--summary` - Human-readable text output instead of DOT
- `-a, --all-deps` - Include implicit dependencies (headers from .d files)

**Examples:**
```bash
# Generate PNG visualization
putup show graph | dot -Tpng -o deps.png

# Text summary
putup show graph --summary

# Include header dependencies
putup show graph --all-deps | dot -Tsvg -o full-deps.svg
```

#### 3.6.4 show var

```
putup show var [NAME] [--json]
```

Show variable assignments and their history. Displays where variables are defined and modified across Tuprules.tup and Tupfile files.

**Arguments:**
- `NAME` - Optional variable name to filter (shows only that variable)

**Options:**
- `--json` - Output in JSON format

**Output format (text):**
```
CC = clang
  History:
    Tuprules.tup:2      CC = gcc
    src/Tupfile:4       CC = clang
  # src/Tupfile:10      CC ?= default   (ineffective)

CFLAGS = -Wall -O2
  History:
    Tuprules.tup:3      CFLAGS = -Wall
    Tuprules.tup:4      CFLAGS += -Wall -O2
```

Lines prefixed with `#` indicate ineffective assignments (e.g., `?=` when the variable was already set).

**Examples:**
```bash
# Show all variables with history
putup show var

# Show specific variable
putup show var CC

# JSON output for tooling
putup show var --json

# Filter specific variable in JSON
putup show var CFLAGS --json
```

**Use cases:**
- Debug why a variable has an unexpected value
- Understand variable inheritance from Tuprules.tup
- Track down where a flag was added or overridden
- Generate variable documentation

#### 3.6.5 show instructions

```
putup show instructions
```

Analyze command instruction deduplication. Shows how many unique instructions exist versus total commands, helping understand index storage efficiency.

**Output:**
```
Instruction Analysis:
  Commands: 147
  Unique instructions: 7
  Deduplication ratio: 21.0x

Top instructions:
  #1 (89 uses): "g++ -std=c++20 -Wall -O2 -c -o %o %f"
  #2 (45 uses): "g++ -std=c++20 -Wall -O2 %f -o %o"
  #3 (8 uses): "ar rcs %o %f"
  ...

Estimated savings: 92% (instruction + operands vs full strings)
```

**Use cases:**
- Verify bang macro effectiveness
- Understand index storage characteristics
- Identify opportunities for macro consolidation

#### 3.6.6 show index

```
putup show index [--summary] [PATTERN]
```

Dump the on-disk `.pup/index` in a grep-friendly form. Unlike other `show` subcommands, this one bypasses Tupfile parsing and reads the index directly — useful for forensics even when the build graph is in a partially-broken state.

**Arguments:**
- `PATTERN` - Optional substring filter; only commands whose command-string contains `PATTERN` are listed.

**Options:**
- `--summary` - Print counts only; skip the per-command listing.

**Output (full dump):**
```
[build] Index: build/.pup/index
  Files:    142
  Commands: 87
  Edges:    412 (Normal=180, Sticky=21, Group=4, Implicit=203, OrderOnly=4)

Commands (with implicit/sticky edges):
  c12  [src]
       cmd: gcc -MD -c -o src/main.o src/main.c
       implicit: src/header.h
       implicit: include/config.h
  c13  [src]
       cmd: gcc -c -o src/util.o src/util.c
       (no implicit/sticky deps)
  ...
```

**Output (`--summary`):**
```
[build] Index: build/.pup/index
  Files:    142
  Commands: 87
  Edges:    412 (Normal=180, Sticky=21, Group=4, Implicit=203, OrderOnly=4)
  Commands with implicit/sticky deps: 64/87
```

**Examples:**
```bash
# Full per-command dump of implicit/sticky deps
putup show index

# Counts only — quick health check of the index
putup show index --summary

# Restrict the per-command listing to commands matching a substring
putup show index main.c
putup show index "ar rcs"
```

**Use cases:**
- Diagnose whether a header's implicit-dep edge was ever recorded (the core forensic question for transitive-dep tracking bugs).
- Sanity-check edge counts after schema changes to the index format.
- Inspect index contents when `putup parse` or `putup build` would otherwise fail.

**Note:** Requires a previously built variant; fails with `No index found` if `.pup/index` is missing.

## 4. Command-Line Options

### 4.1 Global Options

| Option | Long Form | Description |
|--------|-----------|-------------|
| `-j N` | `--jobs N` | Run N jobs in parallel. Default: number of CPU cores. |
| `-k` | `--keep-going` | Continue building after a command fails. |
| | `--no-stat-cache` | Hash every file's contents; skip the size+mtime fast path (see §9.1). |
| | `--rerun` | Re-execute commands even if up to date, scoped by path targets if given (e.g. re-running tests to check for flakes). Results are recorded normally. |
| `-n` | `--dry-run` | Print commands without executing them. |
| `-v` | `--verbose` | Verbose output: show parsing, change detection, etc.; name each finished command by its command line. |
| `-q` | `--quiet` | Name only the commands that fail or write output. |
| `-D VAR=val` | `--define` | Override CONFIG_ variable from CLI. |
| `-S DIR` | | Source directory. Overrides auto-detection. |
| `-C DIR` | `--config-dir` | Config directory (where Tupfiles live). |
| `-B DIR` | | Build/output directory (can use multiple times). |
| `-c FILE` | `--config` | Install FILE as root tup.config (configure command only). |
| `-A` | `--all` | Full project build, ignoring cwd scoping. |
| `-a` | `--all-deps` | Include upstream deps in scoped builds. |
| `-x PATTERN` | `--exclude` | Skip building directories matching PATTERN (repeatable). |
| | `--stat` | Print build statistics after completion. |
| | `--summary` | Human-readable output (for `show graph`). |
| | `--version` | Print version information. |
| `-h` | `--help` | Print help message. |
| | `--` | End of options; remaining arguments are targets. |

**Option Details:**

**`-j, --jobs N`**

Controls parallel execution. Putup runs independent commands concurrently up to the specified limit.

```bash
putup -j1      # Sequential build
putup -j8      # 8 parallel jobs
putup -j$(nproc)  # Use all cores (default behavior)
```

**`-D, --define VAR=value` (Config Override)**

Override CONFIG_ variables from the command line without modifying tup.config files. Follows GCC/CMake conventions.

```bash
putup -D CC=clang              # Override CONFIG_CC
putup -D DEBUG                 # Shorthand for -D DEBUG=y
putup -DDEBUG -DCC=clang       # GCC-style (no space)
putup -D CFLAGS="-O0 -g"       # Values with spaces (quoted)
```

The CONFIG_ prefix is optional and stripped automatically:

```bash
putup -D CC=clang              # Sets CONFIG_CC
putup -D CONFIG_CC=clang       # Same effect
```

CLI overrides have highest precedence, overriding values from tup.config:

```bash
# tup.config has CONFIG_TESTS=y
putup -D TESTS=n show script   # Generate script without tests
```

**`-S DIR` (Source Directory)**

Override automatic project root detection. Useful when running putup from outside the project.

```bash
putup -S /path/to/project
```

**`-C DIR` (Config Directory)**

Specify where Tupfiles live, separate from source files. Enables building third-party code without modification (three-tree builds).

```bash
# Three-tree build: source, config, and output all separate
putup -S busybox -C config -B build

# Shared configs with multiple variants
putup -S busybox -C config -B build-debug
putup -S busybox -C config -B build-release
```

See Section 7.5 for details on three-tree builds.

**`-B DIR` (Build Directory)**

Specify an out-of-tree build directory. All outputs and `.pup/` go here instead of the source tree. Can be specified multiple times to build multiple variants in parallel.

```bash
putup -B build-release    # Build into build-release/
putup clean -B build-release  # Clean that variant

# Multiple variants (built in parallel)
putup -B build-debug -B build-release
```

**No implicit adoption:** Without `-B` flags or targets, putup never selects a variant by scanning. Existing variant directories (subdirs with `tup.config` or `.pup/`) are only listed as hints in the error. Run from inside the variant, or select it explicitly (path targets, a glob, or `-B`).

**`-A, --all` vs `-a, --all-deps`**

These are different options:
- `-A` / `--all` - Disable scoped builds; check all files regardless of cwd
- `-a` / `--all-deps` - Include upstream dependencies in scoped builds

**Scoped Build Behavior (AOSP-style mm/mma):**

Scoped builds always check implicit dependencies (headers discovered from `.d` files), even if those headers live outside the scoped directory. With `-a`, putup also checks explicit upstream dependencies from the build graph (libraries, generated files):

```bash
putup lib        # check lib/ + implicit deps (headers). Like AOSP's mm
putup -a lib     # also check explicit upstream deps (libraries). Like AOSP's mma
```

**`-x, --exclude PATTERN` (Directory Exclusion)**

Skip building directories matching PATTERN. Patterns use gitignore-style
syntax (§6.2) and match a directory or any of its ancestors, so `tests/`
excludes every `tests` directory in the tree.

```bash
putup -x tests/                 # build all, skip every tests/ dir
putup src -x 'src/tests/'       # composes with scoping
putup -x tests/ -x 'bench*/'    # repeatable
```

Exclusion is per-invocation and schedule-level, not discovery-level: excluded
directories are treated like out-of-scope directories in a scoped build. Their
commands keep their index state, so toggling `-x` on and off never causes
spurious rebuilds, and changes under an excluded directory are simply deferred
until a build without `-x`.

`show script`, `show compdb`, and `show graph` respect `-x`; `parse` ignores
it (validation always covers the whole tree).

Example: If `lib/foo.c` includes `../include/header.h` and links `../deps/libutil.a`:
- `putup lib` — changes to `header.h` trigger rebuild (implicit dep, always checked)
- `putup lib` — changes to `libutil.a` are ignored (explicit dep, outside scope)
- `putup -a lib` — changes to `libutil.a` also trigger rebuild

**`--` (End of Options)**

Signals that all remaining arguments are targets, not options or commands. Useful for building directories whose names conflict with commands.

```bash
putup -- build      # Build the 'build' directory as a target
putup -v -- lib     # Verbose build of 'lib' directory
```

### 4.2 Environment Variables

| Variable | Description |
|----------|-------------|
| `PUP_SOURCE_DIR` | Source directory (same as `-S`, lower priority) |
| `PUP_CONFIG_DIR` | Config directory (same as `-C`, lower priority) |
| `PUP_BUILD_DIR` | Build directory (same as `-B`, lower priority) |
| `PUP_IMPLICIT_DEPS` | Set to `0` to disable auto-generated dependency rules (default: enabled) |

**Priority Order:**

For source/build directories:
1. Command-line options (`-S`, `-B`) - highest priority
2. Environment variables (`PUP_SOURCE_DIR`, `PUP_BUILD_DIR`)
3. Enclosing build directory of the current working directory

**`PUP_IMPLICIT_DEPS`**

Controls automatic header dependency discovery via `gcc -M` rules.

```bash
# Disable implicit dependency generation
PUP_IMPLICIT_DEPS=0 putup

# Enable (default)
PUP_IMPLICIT_DEPS=1 putup
```

When enabled, putup auto-generates dependency scanning rules for C/C++ compile commands. See Section 8.2 for details.

## 5. Tupfile Syntax

**Line Continuations:**

A line ending in a backslash continues on the next line. Each byte of the continuation becomes a
space — two for `\`+newline, three for `\`+CRLF — exactly as in tup, so a continued line means the
same as the one-line spelling with those spaces in it:

```tup
: foo.c |> gcc -c \
    -Iinclude \
    %f -o %o |> %B.o
```

The continuation separates words rather than joining them, so `gcc -c\` followed by `%f` runs
`gcc -c foo.c`, not `gcc -cfoo.c`. No command, variable value or path ever contains a newline.

**Comments:**

A line whose first character after any spaces and tabs is `#` is a comment. A `#` later on a line
is text, as in tup: it is part of the name, the value, the path or the command it stands in (a
group or bin name cannot hold one). A continuation joins lines before this test, so a comment
that ends in a backslash takes the next line with it, and a `#` line inside a continued statement
is part of that statement. Write each comment on its own line:

```tup
# Debug information
CFLAGS += -g
```

`CFLAGS += -g  # Debug information` puts `# Debug information` in the value, and the shell then
reads the rest of each command that spells `$(CFLAGS)` as a comment.

### 5.1 Rules

Rules define how to transform inputs into outputs.

**Basic Syntax:**
```
: [foreach] inputs [| order-only] |> command |> outputs [{group}]
```

**Components:**
- `:` - Rule start marker
- `foreach` - Optional; creates one command per input file
- `inputs` - Source files (globs allowed); a rule whose written inputs match no file and name no group produces no command, while a rule written with no inputs runs once
- `| order-only` - Dependencies not included in `%f` (still trigger rebuilds)
- `|>` - Section separator
- `command` - Shell command to execute
- `outputs` - Generated files; the command must create every one of them, or the build fails
- `{group}` - Optional output group membership

**Examples:**

```tup
# Simple rule
: main.c |> gcc -c %f -o %o |> main.o

# Foreach rule (one command per .c file)
: foreach *.c |> gcc -c %f -o %o |> %B.o

# Multiple inputs
: foo.o bar.o |> gcc %f -o %o |> program

# Order-only dependency (not included in %f, but still triggers rebuild)
: main.c | config.h |> gcc -c %f -o %o |> main.o

# Output to a group
: foreach *.c |> gcc -c %f -o %o |> %B.o {objs}

# Using a group as input
: {objs} |> gcc %f -o %o |> program
```

**Cross-Directory Outputs:**

Output paths can use `..` to write files outside the current directory:

```tup
# Output to sibling directory
: foo.c |> gcc -c %f -o %o |> ../build/foo.o

# Output to parent directory
: linux.config |> install -D %f %o |> ../tup.config
```

Output paths are relative to the Tupfile's location in the output tree (for variant builds) or the source tree (for in-tree builds).

An output may not leave the build hierarchy. A rule whose output resolves above the build root is
rejected while the Tupfile is read, and so is an absolute path — including one that points back into
the tree, which never named the file it spelled:

```tup
# Error: Unable to write to a file outside of the build hierarchy: ../escape.txt
: |> echo x > %o |> ../escape.txt

# Error: Unable to write to a file outside of the build hierarchy: /srv/out.txt
: |> echo x > %o |> /srv/out.txt
```

A rule under a config condition is checked whether or not the condition holds, because its outputs
are recorded either way.

Outputs that stay inside the tree are recorded by where they land, so `sub/../gen.txt` and `gen.txt`
name one file.

**Display Text:**

Custom display text replaces the command in output:
```tup
: main.c |> ^ CC %o^ gcc -c %f -o %o |> main.o
# Shows "CC main.o" instead of full command
```

`%` flags expand as they do in the command. When a rule applies a `!macro` that carries its own
display, the macro's wins — it owns the command the display names — and a display written on the
rule is discarded. A failing command is reported by its command line rather than its display, which
would not say what broke. As in tup, `-v` switches from the display to the command line.

### 5.2 Variables

**Assignment:**
```tup
CC = gcc
CFLAGS = -Wall -O2
# Append
CFLAGS += -g
# Same as =
SAME := $(VAR)
# Soft set (if undefined, first wins)
DEFAULT ?= value
# Weak set (if undefined, last wins)
FALLBACK ??= value
```

**Assignment Operators:**

| Operator | Name | Description |
|----------|------|-------------|
| `=` | Set | Assign value (replaces existing) |
| `+=` | Append | Append to existing value (space-separated) |
| `:=` | Define | Same as `=`; the value is expanded at assignment |
| `?=` | Soft Set | Set only if variable is undefined (first wins) |
| `??=` | Weak Set | Deferred default - set at end if undefined (last wins) |

A value runs from its first character that is not a space or a tab to its last one. The spaces and
tabs between them are kept as written, and so are the spaces a line continuation becomes:
`V = a  b` holds two spaces.

**Conditional Assignment Examples:**
```tup
# Soft set - first assignment wins
# Sets CC to "gcc" if not already defined
CC ?= gcc
# Ignored - CC already defined
CC ?= clang

# Weak set - last assignment wins, applied before rules
# Fallback if nothing else sets CFLAGS
CFLAGS ??= -O0
# This wins (last ??= wins)
CFLAGS ??= -O2

# Explicit assignment always wins
# Always sets, ignores any ?= or ??=
CC = clang
```

> **Note:** `?=` and `??=` check if a variable is *defined*, not if it's *empty*.
> An empty string counts as "defined". To provide a default for empty values, use `ifeq`:
> ```tup
> ifeq (@(OS_API),)
> OS_API = posix
> endif
> ```

**Reference:**
```tup
$(CC)                 # Regular variable
@(CONFIG_VAR)         # From tup.config
&(node_var)           # Node variable (advanced)
```

**Built-in Variables:**
| Variable | Description |
|----------|-------------|
| `$(TUP_CWD)` | Relative path from the Tupfile's directory to the directory of the file being parsed: `.` in the Tupfile itself, `../..` inside the root `Tuprules.tup` read from `src/lib`, `../../other` inside `other/inc.tup` included from `src/lib` |
| `$(TUP_PLATFORM)` | Platform: `linux`, `macosx`, `win32` |
| `$(TUP_ARCH)` | Architecture: `x86_64`, `arm`, etc. |
| `$(TUP_VARIANTDIR)` | `$(TUP_CWD)` taken from the variant output dir: the relative path from the Tupfile's source dir to the build-tree counterpart of the directory of the file being parsed (`../../build` inside the root `Tuprules.tup` read from `src/lib`). Equal to `$(TUP_CWD)` in-tree |
| `$(TUP_VARIANT_OUTPUTDIR)` | Relative path from the Tupfile's source dir to its variant output dir (e.g. `../../build/src/lib`; `.` in-tree). Does not change inside included files |
| `$(TUP_SRCDIR)` | Path from where a command runs to the source directory; always `.` |
| `$(TUP_OUTDIR)` | Relative path to output directory (three-tree builds) |

**Examples:**
```tup
CC = gcc
CFLAGS = -Wall -I$(TUP_CWD)/../include

# Use config variable with default
OPTIMIZE = @(OPTIMIZE)
ifeq ($(OPTIMIZE),)
OPTIMIZE = -O2
endif

: foreach *.c |> $(CC) $(CFLAGS) -c %f -o %o |> %B.o
```

### 5.3 Bang Macros

Bang macros define reusable rule templates.

**Definition:**
```tup
!name = |> command |> output-pattern
```

**Invocation:**
```tup
: inputs |> !name |> [outputs]
```

**Examples:**

```tup
# Define a C compiler macro
!cc = |> ^ CC %o^ $(CC) $(CFLAGS) -c %f -o %o |> %B.o

# Define a linker macro
!link = |> ^ LINK %o^ $(CC) $(LDFLAGS) %f -o %o |>

# Use the macros
: foreach *.c |> !cc |> {objs}
: {objs} |> !link |> program
```

**Macro with group output:**
```tup
!cc = |> $(CC) -c %f -o %o |> %B.o {objs}
: foreach *.c |> !cc |>
# Outputs automatically go to {objs} group
```

**Foreach macro:** `foreach` right after the `=`, before any `|` order-only inputs, makes every rule that invokes the macro a foreach rule. Either the macro or the rule can say `foreach`; neither cancels the other.
```tup
!cc = foreach |> $(CC) -c %f -o %o |> %B.o
: *.c |> !cc |>
```

**Alias:** `!name = !target` copies `!target`'s definition, foreach included, as it stands at that line; redefining `!target` later leaves `!name` unchanged, and an undefined `!target` is an error.
```tup
!cc = foreach |> $(CC) -c %f -o %o |> %B.o
!compile = !cc
```

**Empty-input variant:** a rule whose inputs come out empty — a glob that matched nothing, a bin no rule filled — generates no command. When it invokes `!name` and `!name.EMPTY` is defined, it runs `!name.EMPTY` over the empty list instead. Foreach rules never run the variant.
```tup
!ld = |> $(CC) -Wl,-r %f -o %o |>
!ld.EMPTY = |> ar crs %o |>
: {objs} |> !ld |> built-in.o
# With {objs} empty, runs: ar crs built-in.o
```

### 5.4 Pattern Flags

Pattern flags are placeholders expanded at build time.

| Flag | Description | Example |
|------|-------------|---------|
| `%f` | All input files; the rule must have at least one | `gcc %f` → `gcc foo.c bar.c` |
| `%i` | The order-only inputs written after `\|`; the rule must have at least one, and only a command or display string may spell it | `: a.c \| z.h y.h \|> cat %i` → `cat z.h y.h` |
| `%o` | All output files; the rule must declare at least one | `ar rcs %o` → `ar rcs foo.a foo.map` |
| `%O` | The output without its extension, keeping its directory — command string or extra outputs section, and only with exactly one output | `sub/foo.so` → `sub/foo` |
| `%b` | Basename of every input, with extension; the rule must have at least one | `src/foo.c bar.c` → `foo.c bar.c` |
| `%B` | Basename of every input, without extension; the rule must have at least one | `src/foo.c bar.c` → `foo bar` |
| `%e` | Extension of the current file — foreach rules only, and only for files that have one | `foo.c` → `c` |
| `%d` | Name of the Tupfile's directory; at the project root, the project directory's own name | `src/Tupfile` → `src` |
| `%g` | Text the glob matched — rules with exactly one input, and only when a glob produced it | `*_test.c` + `foo_test.c` → `foo` |

**Numbered Inputs/Outputs:**
| Flag | Description |
|------|-------------|
| `%1f` | First input file |
| `%2f` | Second input file |
| `%1b` | Basename of the first input |
| `%1B` | Basename of the first input without its extension |
| `%1i` | First order-only input |
| `%1o` | First output file |
| `%2o` | Second output file |

The number must be at least 1 and below 99 (tup's guard rejects 99 itself, despite its message
naming the range 1-99), a letter must follow it, and that letter must be one of
`f`, `b`, `B`, `o` or `i` — anything else is a parse error rather than literal text. A number
that is in range but names an entry the rule does not have expands to nothing, as in tup.

**Examples:**

```tup
# Foreach: %B is basename of current file
: foreach *.c |> gcc -c %f -o %o |> %B.o
# foo.c → gcc -c foo.c -o foo.o

# Multiple inputs with numbered flags
: header.h template.c |> gen %1f %2f -o %o |> output.c

# Multiple outputs with numbered flags
: input.dat |> split %f -a %1o -b %2o |> part_a.dat part_b.dat

# Using %d for directory-aware output (in src/Tupfile, %d is "src")
: foreach *.c |> gcc -c %f -o %o |> %d-%B.o
# foo.c → src-foo.o

# Using %g for glob match portion
: foreach *_test.c |> run_test %f -o %o |> %g_result.txt
# foo_test.c → run_test foo_test.c -o foo_result.txt
```

### 5.5 Conditionals

Conditionals control which parts of a Tupfile are processed.

**Syntax:**
```tup
ifdef VARIABLE
  # lines if VARIABLE is defined
endif

ifndef VARIABLE
  # lines if VARIABLE is not defined
endif

ifeq ($(VARIABLE),value)
  # lines if VARIABLE equals value
endif

ifneq ($(VARIABLE),value)
  # lines if VARIABLE does not equal value
endif

else
  # optional else clause
endif
```

Whitespace written at the ends of each side is dropped: `ifeq ($(CC), gcc)` equals `ifeq ($(CC),gcc)`. tup keeps the whitespace, so the first form is false there. The value of a variable is compared as it is, with any whitespace it holds.

**`ifdef`/`ifndef` semantics:**

`ifdef NAME` takes a bare variable name (no `$()` or `@()`) and tests *definedness*, not emptiness — a variable set to the empty string counts as defined. Two namespaces are checked in order:

1. **Tupfile variables** (`$(NAME)`) — a deliberate putup extension
2. **Config variables** (`@(NAME)`) — tup-compatible; the `CONFIG_` prefix is optional, so `ifdef FOO` and `ifdef CONFIG_FOO` are equivalent

A Tupfile variable shadows a config variable of the same name. `ifndef` negates the same lookup. Because the fallback reads tup.config definedness, a config-driven conditional can be toggled per invocation without editing any file:

```bash
putup -D LTO        # shorthand for -D LTO=y — makes `ifdef LTO` true
```

> ⚠️ **Differs from tup:** tup's `ifdef` checks only `@`-variables. Putup deliberately checks Tupfile variables first, so a condition can test a variable assigned earlier in the parse. Tupfiles written for tup behave identically unless a tested name is also assigned as a Tupfile variable.

> **Note:** Clearing a config variable in a parent config (`CONFIG_FOO=`, §6.1) leaves it *defined* — `ifdef FOO` remains true.

**Examples:**

```tup
# Platform-specific flags
ifeq ($(TUP_PLATFORM),linux)
  LDFLAGS += -lpthread
endif

# Debug vs release
ifdef DEBUG
  CFLAGS += -g -O0
else
  CFLAGS += -O2
endif

# Check config variable
ifeq (@(ENABLE_TESTS),y)
  : foreach test_*.c |> !cc |> {test_objs}
endif
```

### 5.6 Directives

**`include`** - Include another file:
```tup
include config.tup
include ../common/rules.tup
```

A relative path is resolved against the directory of the file that contains the `include`, so an included file names its neighbours the same way from every Tupfile that includes it.

A file already included under the same conditions is not read again. The conditions are the enclosing `ifeq`/`ifdef` statements on config or environment variables, each with its branch, so a file that includes itself under such a condition is read once more and no further when the configuration takes the branch. In a branch the configuration does not take, an `include` of a file that is being read is not followed.

**`include_rules`** - Include all `Tuprules.tup` files from the project root down to the current directory, in root-first order. Each directory in the path from root to the directory of the file that contains the statement is checked; gaps (directories without a `Tuprules.tup`) are silently skipped. In a Tupfile that is the Tupfile's directory; in an included file it is that file's directory. A file outside the tree that holds the Tupfiles has no such directories, so `include_rules` in it reads nothing. A `Tuprules.tup` already read in the same conditional context is not read again. While `include_rules` reads a `Tuprules.tup`, a second `include_rules` in that file or in a file it includes is an error. That holds in both branches of a condition on a config or environment variable. For `sub/deep/Tupfile`, this is equivalent to:
```tup
# root
include ../../Tuprules.tup
# sub/ (if it exists)
include ../Tuprules.tup
# sub/deep/ (if it exists)
include Tuprules.tup
```

**`export`** - Export variable to command environment:
```tup
export PATH
export CC
export PKG_CONFIG_PATH

# Commands can now see these variables
: foo.c |> $(CC) -c %f -o %o |> foo.o
```

Commands run with a **minimal environment**: on POSIX `PATH`, plus `HOME`,
`TMPDIR`, `TMP` and `TEMP` when putup's own environment sets them to a non-empty
value (Windows passes `PATH` plus the system set: `SystemRoot`, `ComSpec`, `PATHEXT`, `TEMP`, `TMP`, `windir`, and
`HOME` when set). On POSIX an unset `PATH` is replaced by `/usr/bin:/bin`, since a
command with no `PATH` resolves no tool name at all. `HOME` is forwarded because tools
locate their caches and configuration under it — `go build` refuses to run
without it, and a `dash` `/bin/sh` leaves `~` unexpanded. The
temporary-directory variables are forwarded because a command runs with its
Tupfile's directory as cwd, and a tool that finds no writable temporary
directory falls back to cwd, which puts its scratch files in the **source**
tree. Every other variable a command reads must be `export`ed — an unexported
variable is simply absent from the child environment.

Only `export`ed variables are folded into command identity, so changing an
exported variable's **value** re-runs the affected commands even when the
command text is unchanged. The forwarded set above is **not** recorded: a
command whose text reads `$PATH`, `$HOME` or `$TMPDIR` sees whatever the
invoking shell had, and changing that value alone re-runs nothing. `PATH` is the
exception, and only by resolution, never by value: putup resolves the first word
of every command — the first that is not a `NAME=value` assignment, and only when
it contains no `/` — and folds that binary's path, size and mtime into the
command's identity, so a `PATH` change that resolves `gcc` to a different
compiler re-runs the commands that name it, while one that moves nothing they
name re-runs nothing. A tool a command reaches only past a shell operator
(`cd build && gcc ...`), through `sh -c`, or through a wrapper script or compiler
driver is not the first word and is not covered — name it in
`CONFIG_TRACKED_TOOLS` (§6.1). `export` the
variable if its value must decide whether a command is out of date.

**`import`** - Import an environment variable into the Tupfile namespace:
```tup
# Read the CC environment variable (empty if unset)
import CC
# Optional default, used when the env var is unset
import OPTIMIZE = O2
# Same effect — =, ?=, and ??= are equivalent here
import OPTIMIZE ?= O2
```

`import` reads from the process **environment**, not from `tup.config`. It never errors when the variable is unset — the optional default (`=`, `?=`, or `??=`, all equivalent) supplies the value instead. Changing an imported variable's value re-runs the commands that depend on it, because the value is folded into command identity. With `import VAR ?= default`, unsetting the environment variable on a later (warm) build reverts the value to `default` and re-runs the affected commands.

**`error`** - Abort parsing with a message:
```tup
ifeq (@(TOOLCHAIN),)
error CONFIG_TOOLCHAIN must be set
endif
```

The message is variable-expanded and reported as `<file>:<line>: <message>`; parsing stops with a failure. A message-less `error` fails with `Empty error directive`. Guard it with conditionals to validate configuration — an `error` in an inactive branch never fires. A line like `error = x` (any assignment operator) is parsed as an assignment to a variable named `error`, not as a directive; tup would treat the spaced form as a directive with message `= x`.

**`preload`** - Preload a directory for dependency tracking:
```tup
preload ../include
```
> ⚠️ **Parsed, not enforced.** `preload` is accepted for compatibility and ignored, without a warning.

**`.gitignore`** - Generate .gitignore for outputs:
```tup
.gitignore
```
> ⚠️ **Parsed, not enforced.** `.gitignore` is accepted for compatibility and ignored, without a warning; no `.gitignore` file is generated.

### 5.7 Groups

Groups collect outputs for use as inputs to other rules.

**Output Group** - Add outputs to a group:
```tup
: foreach *.c |> gcc -c %f -o %o |> %B.o {objs}
```

**Input Group** - Use a group as input:
```tup
: {objs} |> gcc %f -o %o |> program
```

**Order-Only Group** - Depend on a group for ordering, without pulling its members into `%f` (changes to them still trigger a rebuild):
```tup
: main.c | <generated_headers> |> gcc -c %f -o %o |> main.o
```

**Cross-Directory Groups:**

A `<group>` can be referenced from another directory; an output group `{name}` (tup calls it a
bin) cannot, since it belongs to the Tupfile that fills it. `%<group>` expands to the group's
members:
```tup
# In lib/Tupfile:
: foreach *.c |> !cc |> %B.o <objs>

# In app/Tupfile:
: main.o | ../lib/<objs> |> gcc main.o %<objs> -o %o |> app
```

A `{` starts an output group only at the beginning of a word; inside a word it is part of the
path, so `a{x}.c` names a file and `../lib/{objs}` names a path, not a group.

**Group Naming:**
- `{name}` - Output group (contents are inputs to dependent rules)
- `<name>` - Order-only group (establishes ordering without data dependency)

**Example Project:**

```tup
# Tuprules.tup
ROOT = $(TUP_CWD)
CC = gcc
CFLAGS = -Wall -I$(ROOT)/include
!cc = |> ^ CC %o^ $(CC) $(CFLAGS) -c %f -o %o |> %B.o

# lib/Tupfile
include_rules
: foreach *.c |> !cc |> {lib_objs}
: {lib_objs} |> ar rcs %o %f |> libfoo.a

# app/Tupfile
include_rules
: foreach *.c |> !cc |> {app_objs}
: {app_objs} ../lib/libfoo.a |> $(CC) %f -o %o |> myapp
```

## 6. Configuration

### 6.1 tup.config

The `tup.config` file defines project-wide configuration variables accessible in Tupfiles via `@(VAR)` syntax.

**Location:**
- In-tree builds: `<project_root>/tup.config`
- Variant builds: `<variant_dir>/tup.config`

**Format:**

```ini
# Comment lines start with #
CONFIG_CC=gcc
CONFIG_CFLAGS=-Wall -O2
CONFIG_DEBUG=y
CONFIG_VERSION="1.0.0"
```

**Rules:**
- Variable names must start with `CONFIG_`
- Values can be quoted (quotes are stripped)
- Empty lines and comments (`#`) are ignored

**`CONFIG_TRACKED_TOOLS`** (putup extension) — tool names or paths, separated by
whitespace, whose binaries the build's outputs depend on:

```ini
CONFIG_TRACKED_TOOLS=gcc ld ./scripts/codegen.sh
```

Each entry is resolved (bare names through putup's own `PATH`; entries
containing `/` against the source root) and its
`(path, size, mtime)` is folded into every command's identity. An in-place
toolchain upgrade — same command text, same inputs, different compiler —
then triggers a rebuild, closing the gap that command text alone cannot see.
A missing tool records `<missing>`, so it appearing later also rebuilds.
Putup prints a warning for each listed name that resolves to no file; on Windows a bare
name resolves only when it carries its extension (`gcc.exe`).
An entry that resolves to an output of a rule is an error in every command that parses
the Tupfiles except `configure`: the rule that writes the file would change the
fingerprint on each build, and a rule that reads the file as an input already runs again
when the file changes. An entry that reaches the output only after the output exists (a
symlink to it, or a bare name found through `PATH`) is refused from the next build on.
Unset (the default) tracks only each command's own first word (§ `export`); name
a tool here when a rule reaches it past a shell operator, through `sh -c`, or
through a wrapper or driver, or to catch an in-place upgrade of a tool no command
names directly. The fingerprint trusts size+mtime,
the same trade-off as the stat cache (§9.1).

**CLI Overrides:**

Config variables can be overridden from the command line using `-D`:

```bash
putup -D CC=clang           # Override CONFIG_CC
putup -D DEBUG=n            # Disable CONFIG_DEBUG
putup -DDEBUG -DCC=clang    # Multiple overrides
```

CLI overrides have highest precedence and apply to all scoped configs.

**Usage in Tupfiles:**

```tup
# Access stripped name (recommended)
# Gets value of CONFIG_CC
CC = @(CC)

# Access with default value
DEBUG = @(DEBUG)
ifeq ($(DEBUG),)
DEBUG = n
endif

# Full name also works
CFLAGS = $(CONFIG_CFLAGS)
```

**Example tup.config:**

```ini
# Compiler settings
CONFIG_CC=clang
CONFIG_CXX=clang++
CONFIG_CFLAGS=-Wall -Wextra -O2
CONFIG_LDFLAGS=-flto

# Feature flags
CONFIG_ENABLE_TESTS=y
CONFIG_ENABLE_DEBUG=n

# Build metadata
CONFIG_VERSION="0.1.0"
```

**Fine-Grained Dependency Tracking:**

Pup tracks which commands use which config variables. When a variable changes, only commands that actually reference it (via `@(VAR)` or `$(CONFIG_VAR)`) are rebuilt.

```tup
# Command 1: uses @(CC) and @(CFLAGS)
: foo.c |> @(CC) @(CFLAGS) -c %f -o %o |> foo.o

# Command 2: uses @(CC) and @(LDFLAGS)
: foo.o |> @(CC) %f -o %o @(LDFLAGS) |> program
```

If only `CONFIG_CFLAGS` changes:
- Command 1 rebuilds (uses `@(CFLAGS)`)
- Command 2 does NOT rebuild (doesn't use `@(CFLAGS)`)

This matches tup's fine-grained variable tracking behavior.

**Scoped Config Merging:**

Subdirectories can have their own `tup.config` files. When a Tupfile in `sub/` is parsed, putup collects every `tup.config` from the output root down to `sub/` and merges them. **Parent configs override child configs on collision** — the integrator always wins:

```
build/tup.config           # Integrator overrides (highest precedence)
build/sub/tup.config       # Component defaults (lowest precedence)
```

This is the config counterpart of `Tuprules.tup` `?=` defaults: a component can ship sensible defaults that an integrator overrides simply by setting the same variable in a parent `tup.config`.

```ini
# build/gmp/tup.config — component defaults
CONFIG_CC=gcc
CONFIG_CFLAGS=-O2
```

```ini
# build/tup.config — integrator overrides
CONFIG_CC=clang
```

In `gmp/`, `@(CC)` resolves to `clang` (parent wins) and `@(CFLAGS)` resolves to `-O2` (inherited from child, no collision).

**Precedence (highest to lowest):**

1. `-D` command-line overrides
2. Root `tup.config` (integrator)
3. Intermediate `tup.config` files
4. Leaf `tup.config` (component defaults)

**Clearing a variable:** Set it to empty in the parent config (`CONFIG_FOO=`) to explicitly blank a child's default.

**Empty config files:** An empty `tup.config` is transparent — parent variables merge through it. This differs from the walk-up model where an empty config would block inheritance.

### 6.2 Ignore Patterns

Directory scanning always skips these built-in defaults:

- `.git/`
- `.pup/`
- `node_modules/`

The `-x, --exclude` flag (see Section 4.1) accepts gitignore-style patterns
with the following syntax:

```gitignore
pattern          # Match directories named 'pattern'
pattern/         # Same (trailing slash accepted; all patterns match directories)
path/to/dir      # Anchored pattern (contains /)
```

**Pattern matching:**
| Pattern | Matches |
|---------|---------|
| `tests/` | Directory named `tests`, at any depth |
| `bench*/` | Directories starting with `bench` |
| `src/tests/` | Only `src/tests` (anchored) |
| `**/fixtures` | `fixtures` in any subdirectory |

**Wildcards:**
- `*` - Any characters except `/`
- `**` - Any path segments (including none)
- `?` - Any single character except `/`
- `[abc]` - Character class
- `[a-z]` - Character range

### 6.3 Tupfile.ini

The `Tupfile.ini` file marks the project root. It's the authoritative root marker that stops upward directory traversal.

**Location:** Project root only

**Content:** Can be empty (just needs to exist)

**Optional settings:**

```ini
[tup]
# Future: project-wide settings may go here
```

**Purpose:**
1. **Root detection** - Putup walks up from cwd looking for `Tupfile.ini`
2. **Boundary marker** - Prevents accidental builds in parent directories
3. **Required for out-of-tree builds** - Variant directories reference the source root
4. **Nested project boundary** - A subdirectory with its own `Tupfile.ini` is a
   separate project: discovery prunes it, so its Tupfiles are neither parsed
   nor built as part of the outer project

**Simple projects** can omit `Tupfile.ini` if they have a `Tupfile` at the root, but it's recommended for clarity.

**Composing nested projects:**

A nested project joins the outer build when the outer project *depends* on it —
a rule input referencing a group (or generated file) under the nested root
composes that entire project into the build:

```tup
# gcc/ has its own Tupfile.ini (buildable standalone).
# This reference pulls the whole gcc project into this build:
: gcc/gcc/<xgcc-objs> |> ^ toolchain composed^ touch %o |> .toolchain-composed
```

Composition is at project granularity: one reference anywhere under the nested
root brings in its full subtree (which then builds normally). This is plain tup
syntax — under tup, which parses everything, the same Tupfile behaves
identically. Explicitly targeting a path inside a nested project
(`putup build/gcc/gmp`) also overrides the pruning for that subtree.

See `examples/bsp/` for a working layout: `bsp/` composes the standalone
`gcc/` project while both remain independently buildable.

## 7. Build Modes

### 7.1 In-Tree Builds

The default mode where outputs are generated alongside source files.

**Characteristics:**
- Outputs placed in same directory as Tupfiles
- Single `.pup/` directory at project root
- Simplest setup for small projects

**Setup:**

```
project/
├── Tupfile.ini        # Project root marker
├── Tupfile            # Build rules
├── main.c
├── main.o             # Generated output
└── .pup/              # Index directory
```

**Usage:**

```bash
putup                    # Build
putup clean              # Remove outputs
putup distclean          # Full reset
```

**When to use:**
- Single-configuration projects
- Quick prototyping
- Projects where generated files don't clutter source

### 7.2 Variant Builds

Out-of-tree builds that separate outputs from source files. Multiple variants can coexist (debug, release, cross-compile).

**Setup:**

1. Create a config file:
```ini
# configs/debug.config
CONFIG_CC=gcc
CONFIG_CFLAGS=-g -O0 -DDEBUG
```

2. Create the variant:
```bash
putup configure -B build-debug
# Creates build-debug/ directory and build-debug/tup.config
```

**Result:**

```
project/
├── Tupfile.ini
├── Tupfile
├── main.c
├── configs/
│   ├── debug.config
│   └── release.config
├── build-debug/       # Variant directory
│   ├── tup.config     # Symlink to configs/debug.config
│   ├── main.o         # Output goes here
│   └── .pup/          # Variant's index
└── build-release/     # Another variant
    └── ...
```

**Building variants (path-based selection):**

Specify variant directories directly as targets:

```bash
putup build-debug                  # Build single variant
putup build-debug build-release    # Build multiple variants in parallel
putup build-*                      # Glob pattern - all matching variants
putup *-debug                      # Another glob pattern
```

**Combining variants with scopes:**

Path-based targets can include both variant and scope:

```bash
putup build-debug/src/lib          # Variant + directory scope
putup build-*/src/lib              # Multiple variants + scope
```

**Auto-detection:**

When no targets are specified, putup uses only unambiguous context:

```bash
putup                              # Errors if variant dirs exist but none was
                                   # selected (candidates listed as hints)
putup build-*                      # Builds all matching variants in parallel
cd build-debug && putup            # Builds this variant (any depth inside it)
```

**The -B flag:**

`-B DIR` names a build directory explicitly, and may be repeated:
`putup -B build-debug -B build-release`. A build directory outside the source tree is named this way
(`putup -B /tmp/mybuild`) or through `PUP_BUILD_DIR`.

**Multiple variants:**

```bash
# Create variants (directories created automatically)
putup configure -B build-debug configs
putup configure -B build-release configs
putup configure -B out-arm configs

# Build variants
putup build-debug build-release    # Explicit list
putup build-*                      # Glob pattern
putup -B build-debug -B build-release  # Explicit -B flag
```

**Parallel variant builds:**

When more than one variant is selected (several path targets, a glob, or repeated `-B`), putup
builds them in parallel, one child process per variant, and reports combined results. Every
output line is prefixed with `[variant-name]`.

**Cleaning variants:**

```bash
putup clean -B build-debug      # Remove outputs only
putup distclean -B build-debug  # Remove outputs, .pup/ and tup.config
```

### 7.3 Scoped Builds

Limit builds to specific directories for faster iteration during development.

**How scoping works:**

When you run `putup` from a subdirectory, only rules affecting that directory and its children are considered:

```
project/
├── lib/
│   └── Tupfile        # Compiles lib/*.c
├── app/
│   └── Tupfile        # Compiles app/*.c, links with lib
└── test/
    └── Tupfile        # Compiles tests
```

```bash
cd project/lib
putup                    # Only builds lib/ outputs
```

**Explicit scopes:**

Specify directories as targets:

```bash
putup lib app            # Build lib/ and app/ only
putup test               # Build test/ only
```

**Full builds:**

Use `-A` to ignore scoping and build everything:

```bash
cd project/lib
putup -A                 # Builds entire project despite cwd
```

**Scopes with variants:**

Combine variant selection with directory scopes using path syntax:

```bash
putup build-debug/lib              # Single variant, scoped to lib/
putup build-*/lib                  # All variants, scoped to lib/
```

When targets specify a variant prefix (e.g., `build-debug/lib`), only that variant is built. A target without a variant prefix (e.g., `lib`) scopes whichever build directory is otherwise selected: the one enclosing the working directory, the ones named by `-B` or `PUP_BUILD_DIR`, or the in-tree build.

**Consistency rule:**

All targets must be the same type - either all have explicit variants, or none do:

```bash
# OK - all have variants
putup build-debug/lib build-release/test

# OK - none have variants (applies to all)
putup lib test

# ERROR - mixing variant and non-variant targets
putup build-debug/lib test
```

**Scope behavior:**
- Scoped builds still respect dependencies (if `app/` needs `lib/`, both build)
- Change detection is project-wide, but only scoped commands execute
- Useful for large projects where full builds are slow

### 7.4 Single Output Targets

Target a specific output file to rebuild just that file and its dependencies.

**Syntax:**
```bash
putup build-debug/src/lib/foo.o    # Rebuild single output
```

**How it works:**

1. Putup recognizes the path as a build output (not a source file)
2. Only the command producing that output executes (if inputs changed)
3. Dependencies are still checked and rebuilt if needed

**Requirements:**

- Path must be under a variant directory (e.g., `build-debug/`)
- Path must be a known output in the build graph
- Source files (`.c`, `.cpp`, etc.) are rejected with an error

**Error cases:**

```bash
putup src/main.c           # Error: "src/main.c is a source file, not a build output"
putup build-debug/foo.xyz  # Error: "foo.xyz not in build graph"
```

**Use case:** During development, rebuild just the file you're working on for fast iteration:

```bash
# Make a change to parser.cpp, rebuild just its object file
putup build-debug/src/parser.o

# Run the full link step separately if needed
putup build-debug/myapp
```

### 7.5 Three-Tree Builds (Out-of-Tree Configuration)

Three-tree builds separate source files, Tupfiles, and build outputs into independent directory trees. This enables building third-party code without modification.

**The Three Trees:**

| Tree | Flag | Description |
|------|------|-------------|
| Source | `-S` | Where source files live (can be read-only) |
| Config | `-C` | Where Tupfiles live |
| Output | `-B` | Where outputs and `.pup/` go |

**Use Cases:**

1. **Building third-party code** - Build upstream code (git submodules, vendored sources) without modifying it
2. **Shared configurations** - Same Tupfiles, different `tup.config` per variant
3. **Hermetic builds** - Source, config, and output on different filesystems

**Example: Building Busybox**

```
workspace/
├── busybox/              # Upstream source (read-only, git submodule)
│   ├── coreutils/
│   │   └── cat.c
│   └── shell/
│       └── ash.c
│
├── config/               # Your Tupfiles (mirrors source structure)
│   ├── Tupfile.ini
│   ├── Tuprules.tup
│   ├── coreutils/
│   │   └── Tupfile
│   └── shell/
│       └── Tupfile
│
└── build/                # Build outputs
    ├── tup.config
    ├── .pup/
    ├── coreutils/
    │   └── cat.o
    └── shell/
        └── ash.o
```

**Build command:**
```bash
putup -S busybox -C config -B build
```

**How Paths Resolve:**

| What | Resolved Against |
|------|------------------|
| Glob patterns (`*.c`) | `source_root/current_dir` |
| Tupfile discovery | `config_root` |
| Command working directory | `source_root/current_dir` |
| Output files | `output_root/current_dir` |
| `tup.config` | `output_root` |
| `.pup/index` | `output_root` |

**Variables for Three-Tree Builds:**

Two special variables help Tupfiles reference source and output directories:

| Variable | Description |
|----------|-------------|
| `$(TUP_SRCDIR)` | Path from where a command runs to the source directory; always `.`, since commands run there |
| `$(TUP_OUTDIR)` | Relative path from Tupfile directory to corresponding output directory |

**Example Tupfile:**

```tup
# config/coreutils/Tupfile
include_rules

# In three-tree builds:
# - *.c globs resolve against source_root automatically
# - Outputs go to output_root automatically
# - TUP_SRCDIR/TUP_OUTDIR available for include paths

CFLAGS = -I$(TUP_SRCDIR)/../include

: foreach *.c |> $(CC) $(CFLAGS) -c %f -o %o |> %B.o
```

**Shared Configs with Multiple Variants:**

Same config directory can build multiple variants:

```bash
# Debug variant
putup -S busybox -C config -B build-debug

# Release variant (different tup.config)
putup -S busybox -C config -B build-release
```

Each variant has its own `tup.config` in its output directory.

**Fallback Behavior:**

When `-C` is not specified:
1. If source has `Tupfile.ini` → use source as config root (traditional)
2. If output has `Tupfile.ini` → use output as config root (two-tree)
3. Otherwise → use source as config root (simple projects)

### 7.6 Self-Contained Library Convention

Large projects often contain multiple libraries that share a build (GMP + MPFR + MPC inside GCC, for example). This convention makes each library buildable on its own while staying composable in the larger project.

**The pattern:**

1. **Root `Tuprules.tup`** sets the project layout — toolchain variables and directory names for each library:

```tup
# Root Tuprules.tup
S = $(TUP_CWD)
B = $(TUP_VARIANT_OUTPUTDIR)/$(S)
CC = @(CC)
AR = @(AR)

GMP_DIR = gmp
MPFR_DIR = mpfr
```

2. **Each library's `Tuprules.tup`** provides `?=` defaults for standalone use. In composed mode, root's assignments win and the defaults are no-ops:

```tup
# mpfr/Tuprules.tup
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

3. **Tupfiles** use unprefixed names (`CFLAGS`, `!cc`) — each library's `Tuprules.tup` is only visible to its own subtree via `include_rules`.

**Why directory names are prefixed (`GMP_DIR`, not `DIR`):**

A library that depends on another needs to reference both directories in the same file. MPC's `Tuprules.tup` has `-I$(S)/$(GMP_DIR)`, `-I$(S)/$(MPFR_DIR)/src`, and `-I$(S)/$(MPC_DIR)/src` — three different paths that need three different names. The root also sets all of them in one shared scope, so a single `DIR` variable would collide.

CFLAGS and bang macros (`!cc`, `!gen-config`) don't need prefixes because each library's `Tuprules.tup` is only included by Tupfiles in its own subtree — there's no shared scope where they could collide.

**How `include_rules` enables this:**

`include_rules` includes every `Tuprules.tup` from the project root down to the current directory. For `mpfr/src/Tupfile`, this means root's `Tuprules.tup` runs first (setting `S`, `GMP_DIR = gmp`, `MPFR_DIR = mpfr`), then `mpfr/Tuprules.tup` runs second (its `?=` defaults are no-ops, but it defines `CFLAGS` and `!cc`).

**Standalone vs composed paths:**

```
# Composed (from mpfr/src/, root is ../..)
S = ../..  GMP_DIR = gmp  → $(S)/$(GMP_DIR) = ../../gmp  ✓

# Standalone (mpfr is root, from mpfr/src/)
S = ..     GMP_DIR = ../gmp → $(S)/$(GMP_DIR) = ../../gmp  ✓
```

**Scoped `tup.config` defaults:** Components ship default config values in a `defaults.config` file with a Tupfile copy rule (`cp %f %o`) that installs it as `tup.config` during configure. Parent configs override child configs on collision (see §6.1 *Scoped Config Merging*).

Use `putup parse --check=error` (or its alias `--strict`) to verify that component `Tuprules.tup` files follow these conventions (see §3.2 *Convention checking*). Plain `putup parse` reports the same violations as warnings without failing.

See `examples/bsp/gcc/` for a complete working example with three interdependent libraries.

## 8. Implicit Dependencies

Header files included by C/C++ sources aren't listed in Tupfiles, but changes to them should trigger rebuilds. Putup tracks these "implicit dependencies" automatically.

### 8.1 Compiler .d Files

The recommended method: let the compiler generate dependency information.

**Setup:**

Add `-MD` to your compile flags:

```tup
CFLAGS = -Wall -O2 -MD

: foreach *.c |> $(CC) $(CFLAGS) -c %f -o %o |> %B.o
```

**How it works:**

1. Compiler generates `foo.d` alongside `foo.o`:
   ```makefile
   foo.o: foo.c include/header.h /usr/include/stdio.h
   ```

2. After successful compilation, putup parses the `.d` file

3. Discovered headers stored as implicit edges in the index

4. On subsequent builds, changed headers trigger rebuilds

**Depfile format:**

```makefile
target.o: source.c \
  header1.h \
  path/to/header2.h \
  /usr/include/stdio.h
```

The parser handles:
- Backslash line continuations
- Escaped spaces in paths (`path\ with\ spaces`)
- Windows (CRLF) and Unix (LF) line endings

**Compiler flags:**
| Flag | Effect |
|------|--------|
| `-MD` | Generate `.d` file, continue compilation |
| `-MMD` | Like `-MD` but skip system headers |
| `-MF file` | Write dependencies to specific file |
| `-MT target` | Override target name in `.d` file |

**Recommendation:** Use `-MD` (not `-MMD`) to track system headers too. This catches changes to SDK/toolchain headers during upgrades.

### 8.2 Auto-Generated Dependency Rules

Putup automatically generates dependency scanning commands for C/C++ compiles (enabled by default).

**Disabling:**

```bash
PUP_IMPLICIT_DEPS=0 putup
```

**How it works:**

1. Putup pattern-matches C/C++ compile commands

2. Auto-generates `gcc -M` (or `clang-cl /clang:-M`) rules to discover dependencies

3. Generated rules run before their parent compile commands

4. stdout parsed as depfile, headers added as implicit edges

**Example transformation:**

```tup
# Original rule
: foo.c |> gcc -c %f -o %o |> foo.o
```

Putup generates an internal dependency-scanning rule equivalent to:
```bash
gcc -M -MT foo.o foo.c
```

For `clang-cl` the scan rule uses the MSVC-driver spelling, which emits the same
GNU-format depfile on stdout:
```bash
clang-cl /clang:-M foo.cpp
```

**Pattern matching:**

Recognized compilers: `gcc`, `g++`, `clang`, `clang++`, `cc`, `c++`, `clang-cl`

Recognized wrappers: `ccache`, `distcc`, `sccache`, `icecc`

**The scan carries the compile's flags**, so it preprocesses in the state the compile
does and discovers the same headers a `-MD` depfile would. Only words that would break
the scan itself are left out:

- the compile flag (`-c`, `/c`) and the output flags (`-o` and its argument, `/Fo`,
  `/Fe`, `/Fd`, `/Fi`), which name a product the scan does not make;
- depfile flags (`-M…`, `/clang:-M…`), which would print a second rule over the
  scan's own — though a compile that emits its own depfile is not scanned at all;
- words that change what the driver prints: `/showIncludes` (notes before the rule)
  and `/P` (preprocessed source instead of it);
- `/link`, which hands everything after it to the linker, source words included;
- a word containing a backtick or `$(`, because the scan's shell would evaluate the
  substitution a second time, on results the compile never saw. A `-D` whose value is
  a command substitution is therefore invisible to the scan.

A rule that runs several compiles in one command (`gcc -c a.c … && gcc -c b.c …`) gets
one scan per compile, each carrying that invocation's own flags and sources. Scanning
stops at the first invocation putup cannot reproduce from the rule's directory — a
directory change, an environment assignment, a link, any other program — because past
it the scan would preprocess in a state the compile never had; that invocation and
every one after it go unscanned, and `parse` names each object they leave uncovered.
A build prints how many such objects exist; `-v` names each one in `parse`'s form, on the
build's stdout rather than stderr.
A redirection (`> log`, `2>&1`) divides no invocation, though the scan carries no flag
from beyond it.

Flags whose argument putup recognizes (GNU driver): `-I`, `-isystem`, `-iquote`,
`-include`, `-isysroot`, `--sysroot`, `-D`, `-U`

Recognized argument flags (clang-cl): the above plus `/I`, `/imsvc`, `/external:I`,
`/FI`, `/winsysroot`, `/D`, `/U`

Recognizing an argument means consuming it — a flag spells its argument glued on
(`-Iinc`, `--sysroot=/opt/sys`) or as the next word (`-I inc`, `--sysroot /opt/sys`),
and putup keeps the two forms apart so the next word is not mistaken for a source.
Path arguments are lexically normalized; `-D` and `-U` arguments are passed through
untouched, because a macro value is not a path. Every other flag rides along verbatim.

A scan's stdout must be its dependency rule and nothing else. If it prints anything
more — or prints nothing at all — the build fails and names the scan command: chatter
parsed as dependencies would be recorded as files that never exist, and the command
would re-run for ever.

`cl.exe` (real MSVC) is **not** recognized — it has no GNU-depfile mode. Use
`/sourceDependencies` or a `clang-cl` build if you need implicit deps there.

**When to use each method:**

| Method | Pros | Cons |
|--------|------|------|
| `.d` files (`-MD`) | Explicit, standard, efficient | Requires flag in every compile |
| Auto-gen (default) | Zero Tupfile changes | Slightly slower, pattern-based |

**Recommendation:** Use `-MD` for new projects. Auto-generation works well for adopting existing Tupfiles that don't have `-MD`.

## 9. Incremental Builds

Putup rebuilds only what's necessary by tracking file changes and dependencies in a persistent index.

### 9.1 Change Detection

**What triggers rebuilds:**

| Change | Effect |
|--------|--------|
| Source file modified | Commands using it re-run |
| Tupfile/Tuprules.tup modified | Affected commands re-run |
| Config variable changed | Commands using that variable re-run |
| Header file modified | Commands with implicit deps re-run |
| Command string changed | That command re-runs |
| Output file missing | Command re-runs |

**How changes are detected:**

1. **Size check** (fast path): If file size differs from index, it changed

2. **Stat cache** (fast path): If size *and* mtime both match the index, the
   cached hash is trusted and the file is not re-read. A racy-clean guard
   re-hashes files whose mtime falls within 1s of the last index save.

3. **Hash check** (authoritative): Otherwise, compute SHA-256 and compare.
   A build that is not a dry run records the current mtime of every file it
   hashed and found unchanged, rewriting the index even when there is nothing
   to do, so the next build takes the stat-cache path for it. A file whose
   mtime is within 1s of the build's start, or in the future, is not recorded
   this way: it would still be racy against the new save.

Content-based detection eliminates false positives from `touch file`, editor
saves without changes, or `git checkout` restoring identical content — the
hash matches, so nothing rebuilds.

**Known trade-off:** the stat cache trusts mtime. A change that preserves the
file's size *and* restores its mtime (e.g. `touch -r`, `cp -p` from a
same-size variant, `git checkout` of a same-size revision with an older
timestamp, `rsync --times`) is not detected. Run with `--no-stat-cache` to
skip the fast path and hash every file — slower, but immune to timestamp
manipulation.

**Build flow:**

```
1. PARSE    Re-parse all Tupfiles → fresh in-memory DAG
2. LOAD     Load previous state from .pup/index
3. DIFF     Compare new DAG vs old index:
            - New commands → must run
            - Removed commands → delete stale outputs
            - Changed commands → rebuild
            - Changed inputs → rebuild dependents
4. EXECUTE  Run affected commands (topologically sorted)
5. WRITE    Save complete new index
```

### 9.2 The Index File

Binary file at `.pup/index` storing the complete build state: every file with its size, mtime and
SHA-256 hash, every command with its key and signature, and the edges between them.

The index is a *snapshot*, not a live database. Each build parses every Tupfile, compares the fresh
graph with the previous index, runs what changed, and writes a complete new index in one atomic
rename. The record is also what says which files putup made: stale-output cleanup (§9.3), `clean`
and `distclean` work from it, so reset a build with `putup distclean`, not by deleting the file.

`putup show index` (§3.6) dumps the recorded dependencies. The on-disk layout and the reasons for the
format are in the [Index Module](../DESIGN.md#index-module) section of DESIGN.md; node and link types
are under [Core Types](../DESIGN.md#core-types).

### 9.3 Stale Output Cleanup

When rules are removed or outputs change, putup automatically cleans up stale files.

**Detection:**

During the DIFF phase, putup identifies:
- Commands in index but not in new DAG (rule removed)
- Outputs in index but not in new DAG (output changed)

**Cleanup behavior:**

1. Files generated by removed commands are deleted
2. Old outputs from modified rules are deleted before rebuild
3. Empty parent directories are removed

**Example:**

```tup
# Before: outputs foo.o and bar.o
: foreach *.c |> gcc -c %f -o %o |> %B.o
```

```tup
# After: only outputs foo.o (bar.c deleted)
: foo.c |> gcc -c %f -o %o |> foo.o
```

Putup detects `bar.o` is stale and removes it.

**Manual cleanup:**

```bash
putup clean              # Remove all generated files
putup clean -n           # Dry-run: show what would be removed
```

## 10. Troubleshooting

### 10.1 Common Errors

Syntax errors, `parse` checks, and evaluation errors print as
`file:line[:column]: severity: message`, where severity is `error` or `warning` and the
column appears only for syntax errors. An evaluation error names the statement that failed;
inside an included file, the included file's line. Editors and CI annotators that read
compiler output can jump to the location. Checks that compare rules across the whole
project (a duplicate command, a glob that misses a later directory's output, an input
directory nothing provides) name the rule at fault, the later one for a duplicate. A
message about no particular line, such as a component directory missing its
`Tupfile.ini`, prints as `severity: message`.

**"Not in a pup/tup project"**

```
Error: Not in a pup/tup project (no Tupfile.ini found)
```

Cause: No `Tupfile.ini` or `Tupfile` found in current or parent directories.

Fix: Create `Tupfile.ini` at project root, or cd into the project.

---

**"Dependency cycle detected"**

```
Build failed: Dependency cycle detected
```

Cause: Rules create a dependency cycle.

Fix: Review rules to break the cycle. Use `putup show graph` to visualize.

---

**"already owned by command"**

```
Tupfile:2: error: Unable to create output 'foo.o' because it is already owned by command:
  touch foo.o
```

Cause: Multiple rules produce the same output file.

Fix: Ensure each output is produced by exactly one rule.

---

**"Unable to find bin"**

```
Tupfile:1: error: Unable to find bin 'objs'
```

Cause: A rule uses `{objs}` as input, but no earlier rule in the same Tupfile outputs to it.

Fix: Declare the bin on an earlier rule's output list in the same Tupfile. A bin cannot cross directories; use a `<group>` for that.

---

**"No tup.config found"**

```
Error: No tup.config found. Run 'pup configure' first.
```

Cause: You ran `putup build` without first running `putup configure`.

Fix: Run `putup configure` before building:

```bash
putup configure    # Creates tup.config (runs config rules if any)
putup              # Build
```

For variant builds:

```bash
putup configure -B build-debug    # Creates build-debug/tup.config
putup build-debug                 # Build the variant
```

---

**"expected to write to file"**

```
FAILED: cc -c foo.c -o foo.o
Error: expected to write to file 'foo.o'
```

Cause: The command exited successfully without creating a file the rule declares as an output.
A common source is a command that writes relative to its own working directory while the rule's
output resolves into the build directory.

Fix: Have the command write to the declared path — `%o` expands to it — or correct the rule's
output list to name what the command actually produces.

---

**Command fails but file exists**

Cause: Previous partial build left output file.

Fix: Run `putup clean` then rebuild, or delete the output manually.

### 10.2 Diagnostic Options

**Verbose mode (`-v`)**

Shows detailed information during build:

```bash
putup -v
```

Output includes:
- Each Tupfile as it's parsed
- Variables being set
- Change detection decisions
- Full commands, one line per finished command, in place of their display annotations

Note: Verbose mode disables the ninja-style progress display; each command's line carries the percent complete instead.

**Dry-run (`-n`)**

Print commands without executing:

```bash
putup -n
```

Useful for:
- Seeing what would rebuild
- Checking command expansion
- Verifying after Tupfile changes

**Statistics (`--stat`)**

Print build statistics and performance profiling:

```bash
putup --stat
```

Shows:
- Tupfiles parsed
- Commands (total and executed)
- Files checked and changed
- Files, edges, and implicit deps in index
- Hash computations and stat calls
- The racy-clean window: a file whose recorded mtime falls within it before the last index save is re-hashed rather than trusted
- String pool size (interned strings count and bytes)

**Phase timing breakdown** (for diagnosing slow incremental builds):

```
Stats:
  Tupfiles parsed:         3
  Commands:              117 total, 0 executed
  Files checked:         772 (2 changed)
  Files in index:        856
  Edges in graph:      33962
  Implicit deps:       32227
  Hash computations:     771
  Hashes skipped:        769 (stat cache)
  Stat calls:            772
  Racy-clean window:    1000 ms
  String pool:        366156 strings, 14861508 bytes

  Phase timing:
    Parse:               148.2ms (3 Tupfiles)
    Index load:            2.6ms
    Command index:         0.5ms (0 expansions)
    Change detection:     11.9ms (772 stats, 771 hashes, 769 skipped)
    Implicit deps:         1.1ms
    New commands:          0.5ms
    Stale outputs:         0.0ms
    Job list:              0.8ms
    Command execution:     0.1ms
    Index rebuild:       636.6ms
    Index save:           18.1ms
    Unaccounted:           0.0ms
  Total:                 820.6ms
```

**Phase descriptions:**

| Phase | Description |
|-------|-------------|
| Parse | Discover the project layout and parse Tupfiles into the build graph |
| Index load | Read the previous build's index from disk |
| Command index | Build command string index for lookup |
| Change detection | Stat and hash files to find changes |
| Implicit deps | Expand implicit dependency edges |
| New commands | Detect commands added since last build |
| Stale outputs | Remove outputs from deleted commands |
| Job list | Build topologically-sorted job list |
| Command execution | Run the build commands themselves |
| Index rebuild | Rebuild the in-memory index, carrying implicit edges forward |
| Index save | Write the new index to disk |
| Unaccounted | Total minus the phases above |

The phases are disjoint spans of `Total`, so `Unaccounted` shows at a glance whether
the breakdown explains the run. A large `Unaccounted` means work is happening in a
phase that isn't timed — read it before trusting the rest of the table.

### 10.3 Debug Techniques

**Isolate the problem:**

```bash
# Build single directory
putup lib/

# Build with single job (sequential)
putup -j1

# Clean and rebuild
putup clean && putup
```

**Check what changed:**

```bash
# Dry-run shows what would rebuild
putup -n

# Verbose shows why
putup -v -n
```

`putup show graph` (§3.6) prints the dependency graph as DOT or as a text summary.

**Force full rebuild:**

```bash
putup distclean && putup
```

**Check variable expansion:**

```bash
# Parse only, verbose
putup parse -v
```

## 11. Style Guide

This section covers idiomatic patterns for writing clean, maintainable Tupfiles.

### 11.1 Explicit Source Lists (srcs-y Pattern)

Prefer explicit source file lists over glob patterns. This makes dependencies visible and prevents accidental inclusion of test files or abandoned code.

**Avoid:**
```tup
: foreach src/*.cpp |> !cxx |> {objs}
```

**Prefer:**
```tup
srcs-y  = src/main.cpp
srcs-y += src/parser.cpp
srcs-y += src/lexer.cpp

: foreach $(srcs-y) |> !cxx |> {objs}
```

**Benefits:**
- Explicit control over what gets compiled
- Easy to see all sources at a glance
- Adding/removing files requires conscious decision
- Works well with code review (diffs show intent)

**Naming convention:** The `-y` suffix (from Linux kernel's Kbuild) indicates "yes, compile this". You can extend with conditional lists:

```tup
srcs-y  = core.cpp
srcs-y += parser.cpp

# Platform-specific (see 11.2)
srcs-y += platform-$(OS_API).cpp
```

### 11.2 Platform Selection via Source Names

Use `$(OS_API)` variable substitution in source filenames instead of `ifdef` conditionals.

**Avoid:**
```tup
ifeq ($(OS_API),win32)
  : src/platform_win32.cpp |> !cxx |> platform.o
else
  : src/platform_posix.cpp |> !cxx |> platform.o
endif
```

**Prefer:**
```tup
# In tup.config: CONFIG_OS_API=posix (or win32)
OS_API = @(OS_API)

srcs-y += src/platform-$(OS_API).cpp

: foreach $(srcs-y) |> !cxx |> {objs}
```

**File structure:**
```
src/
├── platform-posix.cpp   # POSIX implementation
├── platform-win32.cpp   # Win32 implementation
└── core.cpp             # Shared code
```

This pattern:
- Eliminates conditional blocks in Tupfiles
- Makes platform variants visible in the filesystem
- Simplifies build rules to single unconditional statements

### 11.3 Config Variables Over Conditionals

Use `@(VAR)` config variables for build options instead of `ifdef` blocks.

**Avoid:**
```tup
ifdef DEBUG
  CFLAGS += -g -O0
else
  CFLAGS += -O2 -DNDEBUG
endif

ifdef USE_MOLD
  LDFLAGS += -fuse-ld=mold
endif
```

**Prefer:**
```tup
# tup.config sets these:
#   CONFIG_DEBUG_CFLAGS=-g -O0
#   CONFIG_RELEASE_CFLAGS=-O2 -DNDEBUG

CFLAGS += @(DEBUG_CFLAGS)
CFLAGS += @(RELEASE_CFLAGS)
LDFLAGS += @(PLATFORM_LDFLAGS)
```

**Config files:**

```ini
# configs/debug.config
CONFIG_OS_API=posix
CONFIG_DEBUG_CFLAGS=-g -O0 -fsanitize=address,undefined
CONFIG_DEBUG_LDFLAGS=-fsanitize=address,undefined

# configs/release.config
CONFIG_OS_API=posix
CONFIG_RELEASE_CFLAGS=-O2 -DNDEBUG -ffunction-sections -fdata-sections
CONFIG_RELEASE_LDFLAGS=-Wl,--gc-sections

# configs/xwin.config (Windows cross-build: clang-cl + xwin, CONFIG=xwin)
CONFIG_OS_API=win32
CONFIG_CC=clang-cl
CONFIG_LINKER=lld-link
CONFIG_CFLAGS=--target=x86_64-pc-windows-msvc /std:c11 /W4
CONFIG_RELEASE_CFLAGS=/O2 /DNDEBUG /MT
```

**Benefits:**
- Build configuration lives in config files, not Tupfile logic
- Switching builds = switching config files
- Tupfiles become simple, declarative

### 11.4 Cross-Compilation (CROSS_COMPILE)

Follow the Linux kernel convention for cross-compilation toolchain prefixes.

**Tuprules.tup:**
```tup
# Import toolchain prefix (empty for native builds)
import CROSS_COMPILE=

# Derive tools from prefix
import CC=$(CROSS_COMPILE)gcc
import CXX=$(CROSS_COMPILE)g++
import AR=$(CROSS_COMPILE)ar
```

**Usage:**
```bash
# Native build
pup

# ARM cross-compile
CROSS_COMPILE=arm-none-eabi- pup -B build-arm

# Windows targets use clang-cl + xwin (CONFIG=xwin), not a CROSS_COMPILE prefix (see §11.5)

# Override specific tool
CROSS_COMPILE=arm-none-eabi- CC=clang pup -B build-arm-clang
```

This convention is understood by embedded developers and integrates with SDK environments that set `CROSS_COMPILE`.

### 11.5 Modular Config Files

Organize build configurations in a `configs/` directory with one file per variant.

**Project structure:**
```
project/
├── Tupfile.ini
├── Tuprules.tup
├── Tupfile
├── configs/
│   ├── default.config    # Default (release, native)
│   ├── debug.config      # Debug with sanitizers
│   ├── release.config    # Optimized release
│   └── xwin.config       # Windows cross-compile (clang-cl + xwin)
└── src/
```

**Creating variants:**
```bash
putup configure -B build-debug --config configs/debug.config
putup configure -B build-release --config configs/release.config
XWIN_SPLAT=~/.xwin/splat CONFIG=xwin putup configure -B build-win   # Windows cross-build (clang-cl + xwin)
```

**Config file template:**
```ini
# configs/example.config
# Build description

# OS API selection (posix or win32)
CONFIG_OS_API=posix

# Build mode flags (mutually exclusive - set one pair)
CONFIG_DEBUG_CFLAGS=-g -O0
CONFIG_DEBUG_CXXFLAGS=-g -O0
CONFIG_DEBUG_LDFLAGS=

# Or release flags:
# CONFIG_RELEASE_CFLAGS=-O2 -DNDEBUG
# CONFIG_RELEASE_CXXFLAGS=-O2 -DNDEBUG
# CONFIG_RELEASE_LDFLAGS=-Wl,--gc-sections

# Platform-specific flags
CONFIG_PLATFORM_LDFLAGS=
```

### 11.6 Bang Macro Design

Define reusable bang macros in `Tuprules.tup` with consistent patterns.

**Good macro design:**
```tup
# Display text with ^ markers for clean output
# %B.o output pattern allows override
!cc = |> ^ CC %f^ $(CC) $(CFLAGS) -c %f -o %o |> %B.o
!cxx = |> ^ CXX %f^ $(CXX) $(CXXFLAGS) -c %f -o %o |> %B.o
!link = |> ^ LINK %o^ $(CXX) %f -o %o $(LDFLAGS) |>
!ar = |> ^ AR %o^ $(AR) rcs %o %f |>

# Variant for third-party code (relaxed warnings)
!cxx_thirdparty = |> ^ CXX %f^ $(CXX) $(CXXFLAGS) -Wno-error -c %f -o %o |> %B.o
```

**Usage in Tupfile:**
```tup
include_rules

: foreach $(srcs-y) |> !cxx |> {objs}
: {objs} |> !ar |> libfoo.a
: $(main-srcs-y) |> !cxx |> main.o
: {objs} main.o |> !link |> program
```

### 11.7 Complete Example

Putting it all together - a well-structured project:

**Tuprules.tup:**
```tup
# S/B Convention: auto-compute source and build root paths
S = $(TUP_CWD)
B = $(TUP_VARIANT_OUTPUTDIR)/$(S)

# OS API
OS_API = @(OS_API)
ifeq ($(OS_API),)
OS_API = posix
endif

# Toolchain
import CROSS_COMPILE=
import CC=$(CROSS_COMPILE)gcc
import CXX=$(CROSS_COMPILE)g++
import AR=$(CROSS_COMPILE)ar

# Flags (use S for source tree, B for build tree)
CFLAGS = -std=c11 -Wall -Wextra -Werror
CXXFLAGS = -std=c++20 -Wall -Wextra -Werror -I$(S)/include

CFLAGS += @(DEBUG_CFLAGS) @(RELEASE_CFLAGS)
CXXFLAGS += @(DEBUG_CXXFLAGS) @(RELEASE_CXXFLAGS)
LDFLAGS += @(DEBUG_LDFLAGS) @(RELEASE_LDFLAGS) @(PLATFORM_LDFLAGS)

# Macros
!cc = |> ^ CC %f^ $(CC) $(CFLAGS) -c %f -o %o |> %B.o
!cxx = |> ^ CXX %f^ $(CXX) $(CXXFLAGS) -c %f -o %o |> %B.o
!link = |> ^ LINK %o^ $(CXX) %f -o %o $(LDFLAGS) |>
!ar = |> ^ AR %o^ $(AR) rcs %o %f |>
```

**Tupfile:**
```tup
include_rules

# Sources (explicit listing)
srcs-y  = src/main.cpp
srcs-y += src/parser.cpp
srcs-y += src/lexer.cpp
srcs-y += src/platform-$(OS_API).cpp

# Build
: foreach $(srcs-y) |> !cxx |> {objs}
: {objs} |> !link |> myapp
```

**configs/default.config:**
```ini
CONFIG_OS_API=posix
CONFIG_RELEASE_CFLAGS=-O2 -DNDEBUG
CONFIG_RELEASE_CXXFLAGS=-O2 -DNDEBUG
CONFIG_RELEASE_LDFLAGS=-Wl,--gc-sections
```
