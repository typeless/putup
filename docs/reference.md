# Pup Reference Manual

## 1. Introduction

### 1.1 What is Pup?

### 1.2 Compatibility with Tup

### 1.3 Key Differences from Tup

## 2. Quick Start

### 2.1 Installation

### 2.2 Your First Build

### 2.3 Project Structure

## 3. Command Reference

### 3.1 pup (build)

```
pup [OPTIONS] [TARGETS...]
```

The default command. Executes the build by parsing Tupfiles, computing the dependency graph, and running commands for changed files.

**Multi-variant auto-detection:** When run from the project root without `-B` flags, pup automatically discovers all variant directories (subdirectories containing `tup.config` or `.pup/`) and builds them in parallel.

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
- `-v` - Verbose output (show parsed files, change detection)
- `-B DIR` - Build directory (can specify multiple times for parallel builds)
- `-A` - Build all files (ignore cwd scoping)
- `--stat` - Print build statistics after completion

**Examples:**
```bash
pup                    # Build from current directory (auto-detects variants)
pup -j8                # Build with 8 parallel jobs
pup -v                 # Verbose build
pup lib app            # Build only lib/ and app/ directories
pup -n                 # Show what would be built

# Path-based variant selection
pup build-debug                  # Build single variant (path-based)
pup build-*                      # Build all variants matching pattern
pup build-debug build-release    # Build specific variants
pup build-debug/src/lib          # Variant + scoped build
pup src/lib                      # Scope applied to all variants
pup build-debug/src/lib/foo.o    # Rebuild single output file

# Explicit -B flag (requires prior configure -B)
pup -B build-debug               # Build specific variant
pup -B build-debug -B build-release  # Multiple -B flags
```

### 3.2 pup parse

```
pup parse [OPTIONS] [TARGETS...]
```

Parse and validate all Tupfiles without executing any commands. Useful for checking syntax errors or seeing what would be built.
Supports path-based variant and scope selection.

**Relevant Options:**
- `-v` - Show each Tupfile as it's parsed
- `-S DIR` - Specify source directory
- `-B DIR` - Specify build directory (can use multiple times)

**Multi-Variant Support:**
- Running from project root auto-detects and parses all variants
- Path-based targets: `pup parse build-debug`, `pup parse build-*`
- Legacy `-B` flag still works for explicit selection

**Examples:**
```bash
pup parse                  # Validate all Tupfiles (auto-detects variants)
pup parse -v               # Show parsing progress
pup parse build-debug      # Parse single variant (path-based)
pup parse build-*          # Parse all matching variants
pup parse build-debug/lib  # Parse scoped to lib/ directory
```

### 3.3 pup clean

```
pup clean [OPTIONS] [TARGETS...]
```

Remove generated output files tracked in the index. Does not remove `.pup/` or `tup.config`.
Supports path-based variant and scope selection.

**Relevant Options:**
- `-n` - Dry-run: show what would be removed
- `-v` - Verbose: list each file removed
- `-B DIR` - Clean a variant build directory (can use multiple times)

**Multi-Variant Support:**
- Running from project root auto-detects and cleans all variants
- Path-based targets: `pup clean build-debug`, `pup clean build-*`
- Legacy `-B` flag still works for explicit selection

**Examples:**
```bash
pup clean                     # Remove generated files (auto-detects variants)
pup clean -n                  # Show what would be removed
pup clean build-debug         # Clean single variant (path-based)
pup clean build-*             # Clean all matching variants
pup clean build-debug/src/lib # Clean scoped to src/lib
```

### 3.4 pup distclean

```
pup distclean [OPTIONS] [TARGETS...]
```

Full reset: remove all generated files, the `.pup/` directory, and `tup.config`. Returns the project to a pristine state.
Supports path-based variant selection.

**Relevant Options:**
- `-n` - Dry-run: show what would be removed
- `-B DIR` - Distclean a variant build directory (can use multiple times)

**Multi-Variant Support:**
- Running from project root auto-detects and distcleans all variants
- Path-based targets: `pup distclean build-debug`, `pup distclean build-*`
- Legacy `-B` flag still works for explicit selection

**Examples:**
```bash
pup distclean             # Full reset (auto-detects variants)
pup distclean build-debug # Reset single variant (path-based)
pup distclean build-*     # Reset all matching variants
```

### 3.5 pup configure

```
pup configure [OPTIONS] [TARGETS...]
```

Execute only rules that output `tup.config` files. Used for two-pass config deployment workflows where config files are generated before the main build.

**Use case:** Multi-project builds where subprojects need per-directory `tup.config` files generated from a central configuration.

**Workflow:**
```bash
pup configure    # Pass 1: Generate tup.config files
pup              # Pass 2: Build with generated configs
```

**How it works:**
1. Parses all Tupfiles using root `tup.config` only
2. Identifies rules where any output ends with `tup.config`
3. Executes only those rules (plus their dependencies)
4. Does not write to `.pup/index` (avoids conflict with subsequent build)

**Relevant Options:**
- `-v` - Verbose output
- `-k` - Continue after failures
- `-n` - Dry-run: show what would execute
- `-B DIR` - Specify build directory (created automatically if it doesn't exist)

**Note:** The `-B` flag creates the output directory if needed. After configure runs, the directory contains `tup.config` which marks it as a variant for subsequent builds. If no config-generating rules exist, an empty `tup.config` is created automatically. The `.pup/` index is NOT created during configure (it's created on first build).

**Important:** You must run `pup configure` before `pup build`. If you skip the configure step, `pup build` will error:

```
Error: No tup.config found. Run 'pup configure' first.
```

This ensures a consistent workflow: configure sets up the build environment, then build executes.

**Example: Pup's own build**

Pup uses `pup configure` for its own build. The `configs/Tupfile` generates `build/tup.config`:

```tup
# configs/Tupfile - Generate tup.config for the variant build

ifeq ($(TUP_PLATFORM),win32)
  CONFIG_FILE = win32.config
else
  CONFIG_FILE = posix.config
endif

: $(CONFIG_FILE) |> install -D %f %o |> ../tup.config
```

Build workflow:
```bash
pup configure -B build   # Generate build/tup.config from configs/posix.config
pup -B build             # Build with generated config
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

### 3.7 pup show

```
pup show <format> [OPTIONS] [TARGETS...]
```

Show build information in various formats. Supports path-based variant and scope selection.

**Formats:**
- `script` - Shell script
- `compdb` - compile_commands.json
- `graph` - DOT format dependency graph

**Examples with targets:**
```bash
pup show graph --summary build-debug    # Single variant
pup show compdb build-*                 # All matching variants
pup show graph build-debug/src/lib      # Variant + scope
```

#### 3.7.1 show script

```
pup show script > build.sh
```

Generate a shell script that runs all build commands in topological order. Useful for environments where pup isn't available or for debugging.

**Output:** Shell script to stdout

#### 3.7.2 show compdb

```
pup show compdb > compile_commands.json
```

Generate a [compilation database](https://clang.llvm.org/docs/JSONCompilationDatabase.html) for IDE integration. Works with clangd, ccls, and other tools.

**Output:** JSON array of compilation commands

**Example usage with clangd:**
```bash
pup show compdb > compile_commands.json
# IDE now has full code intelligence
```

#### 3.7.3 show graph

```
pup show graph [OPTIONS]
```

Show the dependency graph for visualization or analysis.

**Options:**
- `--summary` - Human-readable text output instead of DOT
- `-a, --all-deps` - Include implicit dependencies (headers from .d files)

**Examples:**
```bash
# Generate PNG visualization
pup show graph | dot -Tpng -o deps.png

# Text summary
pup show graph --summary

# Include header dependencies
pup show graph --all-deps | dot -Tsvg -o full-deps.svg
```

## 4. Command-Line Options

### 4.1 Global Options

| Option | Long Form | Description |
|--------|-----------|-------------|
| `-j N` | `--jobs N` | Run N jobs in parallel. Default: number of CPU cores. |
| `-k` | `--keep-going` | Continue building after a command fails. |
| `-n` | `--dry-run` | Print commands without executing them. |
| `-v` | `--verbose` | Verbose output: show parsing, change detection, etc. |
| `-S DIR` | | Source directory. Overrides auto-detection. |
| `-B DIR` | | Build/output directory (can use multiple times). |
| `-A` | `--all` | Full project build, ignoring cwd scoping. |
| `-a` | `--all-deps` | Include upstream deps in scoped builds. |
| | `--stat` | Print build statistics after completion. |
| | `--summary` | Human-readable output (for `show graph`). |
| | `--version` | Print version information. |
| `-h` | `--help` | Print help message. |
| | `--` | End of options; remaining arguments are targets. |

**Option Details:**

**`-j, --jobs N`**

Controls parallel execution. Pup runs independent commands concurrently up to the specified limit.

```bash
pup -j1      # Sequential build
pup -j8      # 8 parallel jobs
pup -j$(nproc)  # Use all cores (default behavior)
```

**`-S DIR` (Source Directory)**

Override automatic project root detection. Useful when running pup from outside the project.

```bash
pup -S /path/to/project
```

**`-B DIR` (Build Directory)**

Specify an out-of-tree build directory. All outputs and `.pup/` go here instead of the source tree. Can be specified multiple times to build multiple variants in parallel.

```bash
pup -B build-release    # Build into build-release/
pup clean -B build-release  # Clean that variant

# Multiple variants (built in parallel)
pup -B build-debug -B build-release
```

**Auto-detection:** Without `-B` flags, pup auto-detects variant directories (subdirs with `tup.config` or `.pup/`) and builds them all in parallel.

**`-A, --all` vs `-a, --all-deps`**

These are different options:
- `-A` / `--all` - Disable scoped builds; check all files regardless of cwd
- `-a` / `--all-deps` - Include upstream dependencies in scoped builds

**Scoped Build Behavior (AOSP-style mm/mma):**

By default, scoped builds only check files within the scope directory (like AOSP's `mm`). With `-a`, pup also checks upstream dependencies (like AOSP's `mma`):

```bash
pup lib        # mm behavior: only check lib/, fast
pup -a lib     # mma behavior: check lib/ + its dependencies
```

Example: If `lib/foo.c` includes `../include/header.h`:
- `pup lib` - changes to `header.h` are ignored
- `pup -a lib` - changes to `header.h` trigger rebuild

**`--` (End of Options)**

Signals that all remaining arguments are targets, not options or commands. Useful for building directories whose names conflict with commands.

```bash
pup -- build      # Build the 'build' directory as a target
pup -v -- lib     # Verbose build of 'lib' directory
```

### 4.2 Environment Variables

| Variable | Description |
|----------|-------------|
| `PUP_SOURCE_DIR` | Source directory (same as `-S`, lower priority) |
| `PUP_BUILD_DIR` | Build directory (same as `-B`, lower priority) |
| `PUP_IMPLICIT_DEPS` | Set to `0` to disable auto-generated dependency rules (default: enabled) |

**Priority Order:**

For source/build directories:
1. Command-line options (`-S`, `-B`) - highest priority
2. Environment variables (`PUP_SOURCE_DIR`, `PUP_BUILD_DIR`)
3. Auto-detection from current working directory

**`PUP_IMPLICIT_DEPS`**

Controls automatic header dependency discovery via `gcc -M` rules.

```bash
# Disable implicit dependency generation
PUP_IMPLICIT_DEPS=0 pup

# Enable (default)
PUP_IMPLICIT_DEPS=1 pup
```

When enabled, pup auto-generates dependency scanning rules for C/C++ compile commands. See Section 8.2 for details.

## 5. Tupfile Syntax

### 5.1 Rules

Rules define how to transform inputs into outputs.

**Basic Syntax:**
```
: [foreach] inputs [| order-only] |> command |> outputs [{group}]
```

**Components:**
- `:` - Rule start marker
- `foreach` - Optional; creates one command per input file
- `inputs` - Source files (globs allowed)
- `| order-only` - Dependencies not included in `%f` (still trigger rebuilds)
- `|>` - Section separator
- `command` - Shell command to execute
- `outputs` - Generated files
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
: posix.config |> install -D %f %o |> ../tup.config
```

Output paths are relative to the Tupfile's location in the output tree (for variant builds) or the source tree (for in-tree builds).

**Display Text:**

Custom display text replaces the command in output:
```tup
: main.c |> ^ CC %o^ gcc -c %f -o %o |> main.o
# Shows "CC main.o" instead of full command
```

### 5.2 Variables

**Assignment:**
```tup
CC = gcc
CFLAGS = -Wall -O2
CFLAGS += -g          # Append
LITERAL := $(VAR)     # No expansion (literal string)
```

**Reference:**
```tup
$(CC)                 # Regular variable
@(CONFIG_VAR)         # From tup.config
&(node_var)           # Node variable (advanced)
```

**Built-in Variables:**
| Variable | Description |
|----------|-------------|
| `$(TUP_CWD)` | Current Tupfile directory (relative to root) |
| `$(TUP_ROOT)` | Path to project root from current directory |
| `$(TUP_PLATFORM)` | Platform: `linux`, `macosx`, `win32` |
| `$(TUP_ARCH)` | Architecture: `x86_64`, `arm`, etc. |
| `$(TUP_VARIANTDIR)` | Variant output directory (variant builds) |

**Examples:**
```tup
CC = gcc
CFLAGS = -Wall -I$(TUP_ROOT)/include

# Use config variable with default
OPTIMIZE = @(OPTIMIZE:-O2)

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

### 5.4 Pattern Flags

Pattern flags are placeholders expanded at build time.

| Flag | Description | Example |
|------|-------------|---------|
| `%f` | All input files | `gcc %f` → `gcc foo.c bar.c` |
| `%o` | All output files | `-o %o` → `-o foo.o` |
| `%b` | Basename with extension | `foo.c` → `foo.c` |
| `%B` | Basename without extension | `foo.c` → `foo` |
| `%e` | Extension only (foreach) | `foo.c` → `c` |
| `%d` | Directory name | `src/foo.c` → `src` |
| `%g` | Glob match portion | `*.c` matching `foo.c` → `foo.c` |

**Numbered Inputs:**
| Flag | Description |
|------|-------------|
| `%1f` | First input file |
| `%2f` | Second input file |
| `%1o` | First output file |

**Examples:**

```tup
# Foreach: %B is basename of current file
: foreach *.c |> gcc -c %f -o %o |> %B.o
# foo.c → gcc -c foo.c -o foo.o

# Multiple inputs with numbered flags
: header.h template.c |> gen %1f %2f -o %o |> output.c

# Using %d for directory-aware output
: foreach src/*.c |> gcc -c %f -o %o |> %d/%B.o
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

**`include_rules`** - Include Tuprules.tup from current and parent directories:
```tup
include_rules
# Searches upward for Tuprules.tup files
```

**`export`** - Export variable to command environment:
```tup
export PATH
export CC
export PKG_CONFIG_PATH

# Commands can now see these variables
: foo.c |> $(CC) -c %f -o %o |> foo.o
```

**`import`** - Import from tup.config:
```tup
import CC               # Required, error if not in config
import OPTIMIZE=O2      # Optional with default
```

**`preload`** - Preload a directory for dependency tracking:
```tup
preload ../include
```

**`.gitignore`** - Generate .gitignore for outputs:
```tup
.gitignore
```

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

**Order-Only Group** - Depend on group without triggering rebuild:
```tup
: main.c | <generated_headers> |> gcc -c %f -o %o |> main.o
```

**Cross-Directory Groups:**

Groups can be referenced across directories:
```tup
# In lib/Tupfile:
: foreach *.c |> !cc |> %B.o {objs}

# In app/Tupfile:
: ../lib/{objs} main.o |> !link |> app
```

**Group Naming:**
- `{name}` - Output group (contents are inputs to dependent rules)
- `<name>` - Order-only group (establishes ordering without data dependency)

**Example Project:**

```tup
# Tuprules.tup
CC = gcc
CFLAGS = -Wall -I$(TUP_ROOT)/include
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

**Usage in Tupfiles:**

```tup
# Access stripped name (recommended)
CC = @(CC)              # Gets value of CONFIG_CC

# Access with default value
DEBUG = @(DEBUG:-n)     # Use "n" if CONFIG_DEBUG not set

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

### 6.2 .pupignore / .tupignore

Ignore files specify directories and files that pup should skip during scanning.

**Location:** Project root (`.pupignore` or `.tupignore`)

**Default ignores** (always applied):
- `.git/`
- `.pup/`
- `node_modules/`

**Syntax:**

```gitignore
# Comment
pattern          # Ignore matching files/directories
pattern/         # Directory only (trailing slash)
!pattern         # Negation (un-ignore)
path/to/file     # Anchored pattern (contains /)
```

**Pattern matching:**
| Pattern | Matches |
|---------|---------|
| `*.o` | Any `.o` file in any directory |
| `build/` | Directory named `build` |
| `src/*.o` | `.o` files directly in `src/` (anchored) |
| `**/test` | `test` in any subdirectory |
| `!important.o` | Keep `important.o` even if `*.o` ignored |

**Wildcards:**
- `*` - Any characters except `/`
- `**` - Any path segments (including none)
- `?` - Any single character except `/`
- `[abc]` - Character class
- `[a-z]` - Character range

**Example .pupignore:**

```gitignore
# Build artifacts
*.o
*.a
*.so

# IDE files
.vscode/
.idea/
*.swp

# Specific directories
build/
out/
third_party/

# Keep this one
!third_party/catch.hpp
```

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
1. **Root detection** - Pup walks up from cwd looking for `Tupfile.ini`
2. **Boundary marker** - Prevents accidental builds in parent directories
3. **Required for out-of-tree builds** - Variant directories reference the source root

**Simple projects** can omit `Tupfile.ini` if they have a `Tupfile` at the root, but it's recommended for clarity.

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
pup                    # Build
pup clean              # Remove outputs
pup distclean          # Full reset
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
pup configure -B build-debug configs
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
pup build-debug                  # Build single variant
pup build-debug build-release    # Build multiple variants in parallel
pup build-*                      # Glob pattern - all matching variants
pup *-debug                      # Another glob pattern
```

**Combining variants with scopes:**

Path-based targets can include both variant and scope:

```bash
pup build-debug/src/lib          # Variant + directory scope
pup build-*/src/lib              # Multiple variants + scope
```

**Auto-detection:**

When no targets are specified, pup auto-detects variant directories:

```bash
pup                              # Builds all discovered variants in parallel
cd build-debug && pup            # Builds only this variant
```

**Legacy -B flag:**

The `-B` flag is still supported for:
- Creating new out-of-tree builds: `pup -B /tmp/mybuild`
- Explicit variant selection: `pup -B build-debug -B build-release`

Path-based selection is preferred for existing variants.

**Multiple variants:**

```bash
# Create variants (directories created automatically)
pup configure -B build-debug configs
pup configure -B build-release configs
pup configure -B out-arm configs

# Build variants
pup build-debug build-release    # Explicit list
pup build-*                      # Glob pattern
pup -B build-debug -B build-release  # Explicit -B flag
```

**Parallel variant builds:**

When run from the project root without `-B` flags, pup automatically:
1. Discovers all variant directories (subdirs with `tup.config` or `.pup/`)
2. Builds them in parallel using `std::async`
3. Reports combined results

In verbose mode (`-v`), output lines are prefixed with `[variant-name]` to distinguish which variant produced each message.

**Cleaning variants:**

```bash
pup clean -B build-debug      # Remove outputs only
pup distclean -B build-debug  # Remove entire variant directory
```

### 7.3 Scoped Builds

Limit builds to specific directories for faster iteration during development.

**How scoping works:**

When you run `pup` from a subdirectory, only rules affecting that directory and its children are considered:

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
pup                    # Only builds lib/ outputs
```

**Explicit scopes:**

Specify directories as targets:

```bash
pup lib app            # Build lib/ and app/ only
pup test               # Build test/ only
```

**Full builds:**

Use `-A` to ignore scoping and build everything:

```bash
cd project/lib
pup -A                 # Builds entire project despite cwd
```

**Scopes with variants:**

Combine variant selection with directory scopes using path syntax:

```bash
pup build-debug/lib              # Single variant, scoped to lib/
pup build-*/lib                  # All variants, scoped to lib/
pup lib                          # All variants, scoped to lib/ (shorthand)
```

When targets specify a variant prefix (e.g., `build-debug/lib`), only that variant is built. Without a variant prefix (e.g., `lib`), the scope applies to all discovered variants.

**Consistency rule:**

All targets must be the same type - either all have explicit variants, or none do:

```bash
# OK - all have variants
pup build-debug/lib build-release/test

# OK - none have variants (applies to all)
pup lib test

# ERROR - mixing variant and non-variant targets
pup build-debug/lib test
```

**Scope behavior:**
- Scoped builds still respect dependencies (if `app/` needs `lib/`, both build)
- Change detection is project-wide, but only scoped commands execute
- Useful for large projects where full builds are slow

### 7.4 Single Output Targets

Target a specific output file to rebuild just that file and its dependencies.

**Syntax:**
```bash
pup build-debug/src/lib/foo.o    # Rebuild single output
```

**How it works:**

1. Pup recognizes the path as a build output (not a source file)
2. Only the command producing that output executes (if inputs changed)
3. Dependencies are still checked and rebuilt if needed

**Requirements:**

- Path must be under a variant directory (e.g., `build-debug/`)
- Path must be a known output in the build graph
- Source files (`.c`, `.cpp`, etc.) are rejected with an error

**Error cases:**

```bash
pup src/main.c           # Error: "src/main.c is a source file, not a build output"
pup build-debug/foo.xyz  # Error: "foo.xyz not in build graph"
```

**Use case:** During development, rebuild just the file you're working on for fast iteration:

```bash
# Make a change to parser.cpp, rebuild just its object file
pup build-debug/src/parser.o

# Run the full link step separately if needed
pup build-debug/myapp
```

## 8. Implicit Dependencies

Header files included by C/C++ sources aren't listed in Tupfiles, but changes to them should trigger rebuilds. Pup tracks these "implicit dependencies" automatically.

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

2. After successful compilation, pup parses the `.d` file

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

Pup automatically generates dependency scanning commands for C/C++ compiles (enabled by default).

**Disabling:**

```bash
PUP_IMPLICIT_DEPS=0 pup
```

**How it works:**

1. Pup pattern-matches C/C++ compile commands

2. Auto-generates `gcc -M` rules to discover dependencies

3. Generated rules run before their parent compile commands

4. stdout parsed as depfile, headers added as implicit edges

**Example transformation:**

```tup
# Original rule
: foo.c |> gcc -c %f -o %o |> foo.o
```

Pup generates an internal dependency-scanning rule equivalent to:
```bash
gcc -M -MT foo.o foo.c
```

**Pattern matching:**

Recognized compilers: `gcc`, `g++`, `clang`, `clang++`, `cc`, `c++`

Recognized wrappers: `ccache`, `distcc`, `sccache`, `icecc`

Preserved flags: `-I`, `-D`, `-U`, `-std=`, `-isystem`, `--sysroot`

**When to use each method:**

| Method | Pros | Cons |
|--------|------|------|
| `.d` files (`-MD`) | Explicit, standard, efficient | Requires flag in every compile |
| Auto-gen (default) | Zero Tupfile changes | Slightly slower, pattern-based |

**Recommendation:** Use `-MD` for new projects. Auto-generation works well for adopting existing Tupfiles that don't have `-MD`.

## 9. Incremental Builds

Pup rebuilds only what's necessary by tracking file changes and dependencies in a persistent index.

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

2. **Hash check**: If size matches, compute SHA-256 hash and compare

This content-based detection eliminates false positives from:
- `touch file` (timestamp changes, content unchanged)
- `git checkout` (restores old timestamp)
- `rsync` (may preserve timestamps)
- Editor save without changes

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

Binary file at `.pup/index` storing the complete build state.

**Contents:**

| Section | Description |
|---------|-------------|
| Header (40 bytes) | Magic number, version, counts, offsets |
| File entries (56 bytes each) | Parent, name offset, type, size, SHA-256 hash |
| Command entries (16 bytes each) | Dir ID, command/display/env offsets |
| Edges (16 bytes each) | From, to, link type, group cmd ID |
| String table | Length-prefixed packed strings |
| Footer (32 bytes) | SHA-256 checksum |

**Link types:**

| Type | Meaning |
|------|---------|
| Normal | Input/output relationship |
| Sticky | Explicit dependency from Tupfile or config variable |
| Group | Membership in output group |
| Implicit | Header dependency from `.d` file |

**Node types:**

| Type | Description |
|------|-------------|
| File | Source file in the project |
| Generated | Output file produced by a command |
| Ghost | Placeholder for file referenced before it exists (cross-directory dependencies) |
| Directory | Directory node (parent for path resolution) |
| Command | Build command to execute |
| Variable | Config variable from tup.config |

**Note on Ghost nodes:** Ghosts are created during parsing when a rule references a file that doesn't exist yet (common in variant builds where directories are parsed alphabetically). When the producing rule is later parsed, the Ghost is upgraded to Generated. Ghosts are never written to the index—they're transient during parsing only.

**Key design:**

The index is a *snapshot*, not a live database. Each build writes a complete new index file. This is efficient because:

- Serialization is fast (simple binary format)
- Only changed parts of the *build* execute
- Single atomic write at the end

**Path storage:**

Paths use a (parent_id, name) model like tup's database:
- Only basename stored per entry
- Full paths reconstructed by walking parent chain
- Enables O(1) lookup by directory + name

**Tagged ID spaces (index format v7):**

Files and commands occupy separate ID spaces for O(1) lookup:
- File IDs: 1, 2, 3, ... (stored in dense array, ID = array_index + 1)
- Command IDs: 0x80000001, 0x80000002, ... (high bit set)
- ID field removed from on-disk format (computed from array position)
- Lookup: `is_command_id(id) ? commands_[id & ~0x80000000 - 1] : files_[id - 1]`

### 9.3 Stale Output Cleanup

When rules are removed or outputs change, pup automatically cleans up stale files.

**Detection:**

During the DIFF phase, pup identifies:
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

Pup detects `bar.o` is stale and removes it.

**Manual cleanup:**

```bash
pup clean              # Remove all generated files
pup clean -n           # Dry-run: show what would be removed
```

## 10. Troubleshooting

### 10.1 Common Errors

**"Not in a pup/tup project"**

```
Error: Not in a pup/tup project (no Tupfile.ini found)
```

Cause: No `Tupfile.ini` or `Tupfile` found in current or parent directories.

Fix: Create `Tupfile.ini` at project root, or cd into the project.

---

**"Circular dependency detected"**

```
Error: Circular dependency: a.o -> b.o -> a.o
```

Cause: Rules create a dependency cycle.

Fix: Review rules to break the cycle. Use `pup show graph` to visualize.

---

**"Output already defined"**

```
Error: Output 'foo.o' already defined by another rule
```

Cause: Multiple rules produce the same output file.

Fix: Ensure each output is produced by exactly one rule.

---

**"Unknown variable"**

```
Error: Unknown variable: $(UNDEFINED_VAR)
```

Cause: Variable referenced but never assigned.

Fix: Define the variable, or use a default: `$(VAR:-default)`

---

**"Group not found"**

```
Error: Group {objs} referenced but not defined
```

Cause: Using a group as input before any rule outputs to it.

Fix: Ensure rules outputting to the group are in scope.

---

**"No tup.config found"**

```
Error: No tup.config found. Run 'pup configure' first.
```

Cause: You ran `pup build` without first running `pup configure`.

Fix: Run `pup configure` before building:

```bash
pup configure    # Creates tup.config (runs config rules if any)
pup              # Build
```

For variant builds:

```bash
pup configure -B build-debug    # Creates build-debug/tup.config
pup build-debug                 # Build the variant
```

---

**Command fails but file exists**

Cause: Previous partial build left output file.

Fix: Run `pup clean` then rebuild, or delete the output manually.

### 10.2 Diagnostic Options

**Verbose mode (`-v`)**

Shows detailed information during build:

```bash
pup -v
```

Output includes:
- Each Tupfile as it's parsed
- Variables being set
- Change detection decisions
- Commands being executed

**Dry-run (`-n`)**

Print commands without executing:

```bash
pup -n
```

Useful for:
- Seeing what would rebuild
- Checking command expansion
- Verifying after Tupfile changes

**Statistics (`--stat`)**

Print build statistics:

```bash
pup --stat
```

Shows:
- Total files/commands
- Files changed
- Commands executed
- Build time

**Graph visualization**

Visualize dependencies:

```bash
# DOT format for graphviz
pup show graph | dot -Tpng -o deps.png

# Text summary
pup show graph --summary

# Include header dependencies
pup show graph --all-deps
```

### 10.3 Debug Techniques

**Isolate the problem:**

```bash
# Build single directory
pup lib/

# Build with single job (sequential)
pup -j1

# Clean and rebuild
pup clean && pup
```

**Check what changed:**

```bash
# Dry-run shows what would rebuild
pup -n

# Verbose shows why
pup -v -n
```

**Inspect the graph:**

```bash
# Text summary of all rules
pup show graph --summary

# Visual graph (requires graphviz)
pup show graph | dot -Tsvg -o graph.svg
```

**Force full rebuild:**

```bash
# Remove index, keep outputs
rm -rf .pup/index

# Or clean everything
pup distclean && pup
```

**Check variable expansion:**

```bash
# Parse only, verbose
pup parse -v
```

**Compare with tup:**

If migrating from tup, run both and compare:

```bash
# Build with pup
pup -n > pup-commands.txt

# Build with tup (if available)
tup -n > tup-commands.txt

diff pup-commands.txt tup-commands.txt
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

# configs/win32.config
CONFIG_OS_API=win32
CONFIG_RELEASE_CFLAGS=-O2 -DNDEBUG
CONFIG_PLATFORM_LDFLAGS=-static
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

# MinGW cross-compile
CROSS_COMPILE=x86_64-w64-mingw32- pup -B build-win32

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
│   └── win32.config      # Windows cross-compile
└── src/
```

**Creating variants:**
```bash
pup variant configs/debug.config      # Creates build-debug/
pup variant configs/release.config    # Creates build-release/
pup variant configs/win32.config      # Creates build-win32/
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
OS_API ?= posix

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

## Appendices

### A. Tup Compatibility Matrix

| Feature | Tup | Pup | Notes |
|---------|-----|-----|-------|
| **Core Syntax** |
| Basic rules | ✅ | ✅ | |
| foreach rules | ✅ | ✅ | |
| Bang macros | ✅ | ✅ | |
| Variables | ✅ | ✅ | |
| Config variables (@) | ✅ | ✅ | |
| Node variables (&) | ✅ | ⚠️ | Partial |
| Conditionals | ✅ | ✅ | |
| Groups | ✅ | ✅ | |
| Order-only deps | ✅ | ✅ | |
| **Directives** |
| include | ✅ | ✅ | |
| include_rules | ✅ | ✅ | |
| export | ✅ | ✅ | |
| import | ✅ | ✅ | |
| preload | ✅ | ✅ | |
| run | ✅ | ❌ | Shell execution during parse |
| .gitignore | ✅ | ✅ | |
| **Commands** |
| build | ✅ | ✅ | |
| init | ✅ | ✅ | |
| parse | ✅ | ✅ | |
| upd | ✅ | ➡️ | Alias for build |
| variant | ✅ | ✅ | |
| monitor | ✅ | ❌ | Filesystem watch daemon |
| graph | ✅ | ✅ | Via `show graph` |
| **Features** |
| FUSE sandbox | ✅ | ❌ | Pup uses index-based tracking |
| Lua scripting | ✅ | ❌ | Not planned |
| SQLite database | ✅ | ❌ | Pup uses binary index |
| Parallel builds | ✅ | ✅ | |
| Incremental builds | ✅ | ✅ | |
| Cross-platform | ✅ | ✅ | Linux, macOS, Windows |
| **Pup Extensions** |
| Path-based variant selection | ❌ | ✅ | `pup build-debug` vs `-B` flag |
| Glob variant patterns | ❌ | ✅ | `pup build-*` |
| Single output targets | ❌ | ✅ | `pup build-debug/foo.o` |
| Multi-variant parallel | ❌ | ✅ | Auto-detect and build variants |
| show script | ❌ | ✅ | Generate build.sh |
| show compdb | ❌ | ✅ | compile_commands.json |
| Content-based hashing | ❌ | ✅ | SHA-256 for change detection |

**Legend:** ✅ Supported | ⚠️ Partial | ❌ Not supported | ➡️ Different name

### B. Pattern Flags Reference

**Input Flags:**

| Flag | Description | Example Input | Result |
|------|-------------|---------------|--------|
| `%f` | All inputs | `foo.c bar.c` | `foo.c bar.c` |
| `%b` | Basename with ext | `src/foo.c` | `foo.c` |
| `%B` | Basename no ext | `src/foo.c` | `foo` |
| `%e` | Extension only | `foo.c` | `c` |
| `%d` | Directory | `src/foo.c` | `src` |
| `%g` | Glob match | `*.c` → `foo.c` | `foo.c` |

**Output Flags:**

| Flag | Description |
|------|-------------|
| `%o` | All outputs |

**Numbered Flags:**

| Flag | Description |
|------|-------------|
| `%1f` | First input |
| `%2f` | Second input |
| `%3f` | Third input (etc.) |
| `%1o` | First output |
| `%2o` | Second output (etc.) |

**Usage Examples:**

```tup
# Basic: %f for inputs, %o for outputs
: foo.c |> gcc -c %f -o %o |> foo.o

# Foreach: %B expands per-file
: foreach *.c |> gcc -c %f -o %o |> %B.o

# Numbered: specific input positions
: header.h source.c |> process %1f %2f -o %o |> output.c

# Directory-aware output
: foreach src/*.c |> gcc -c %f -o %o |> obj/%B.o
```

### C. Environment Variables Reference

**Pup Configuration:**

| Variable | Description | Default |
|----------|-------------|---------|
| `PUP_SOURCE_DIR` | Source directory | Auto-detect |
| `PUP_BUILD_DIR` | Build/output directory | Source dir |
| `PUP_IMPLICIT_DEPS` | Enable auto dep scanning | `1` (on) |

**Tupfile Built-ins:**

| Variable | Description | Example |
|----------|-------------|---------|
| `$(TUP_CWD)` | Current Tupfile dir (relative) | `src/lib` |
| `$(TUP_ROOT)` | Path to root from current dir | `../..` |
| `$(TUP_PLATFORM)` | Platform name | `linux`, `macosx`, `win32` |
| `$(TUP_ARCH)` | CPU architecture | `x86_64`, `arm`, `aarch64` |
| `$(TUP_VARIANTDIR)` | Variant output dir | `../build-debug/src` |

**Priority Order:**

For source/build directories:
1. Command-line (`-S`, `-B`) — highest
2. Environment (`PUP_SOURCE_DIR`, `PUP_BUILD_DIR`)
3. Auto-detection from cwd — lowest

**Example Usage:**

```bash
# Build with specific directories
PUP_SOURCE_DIR=/path/to/src PUP_BUILD_DIR=/path/to/build pup

# Enable implicit dependency scanning
PUP_IMPLICIT_DEPS=1 pup

# Override via command line (higher priority)
PUP_SOURCE_DIR=/wrong pup -S /correct  # Uses /correct
```
