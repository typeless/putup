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

**Arguments:**
- `TARGETS` - Optional directory paths to scope the build (see Section 7.3)

**Relevant Options:**
- `-j N` - Run N jobs in parallel (default: number of CPUs)
- `-k` - Continue building after failures
- `-n` - Dry-run: print commands without executing
- `-v` - Verbose output (show parsed files, change detection)
- `-A` - Build all files (ignore cwd scoping)
- `--stat` - Print build statistics after completion

**Examples:**
```bash
pup                    # Build from current directory
pup -j8                # Build with 8 parallel jobs
pup -v                 # Verbose build
pup lib app            # Build only lib/ and app/ directories
pup -n                 # Show what would be built
```

### 3.2 pup init

```
pup init
```

Initialize a `.pup` directory in the current project. Creates the directory structure needed for pup's index and configuration.

**Notes:**
- Automatically called by `pup build` if `.pup` doesn't exist
- Creates `.pup/` in the output root (same as source root for in-tree builds)

### 3.3 pup parse

```
pup parse [OPTIONS]
```

Parse and validate all Tupfiles without executing any commands. Useful for checking syntax errors or seeing what would be built.

**Relevant Options:**
- `-v` - Show each Tupfile as it's parsed
- `-S DIR` - Specify source directory
- `-B DIR` - Specify build directory

**Examples:**
```bash
pup parse              # Validate all Tupfiles
pup parse -v           # Show parsing progress
```

### 3.4 pup clean

```
pup clean [OPTIONS]
```

Remove generated output files tracked in the index. Does not remove `.pup/` or `tup.config`.

**Relevant Options:**
- `-n` - Dry-run: show what would be removed
- `-v` - Verbose: list each file removed
- `-B DIR` - Clean a variant build directory

**Examples:**
```bash
pup clean              # Remove generated files
pup clean -n           # Show what would be removed
pup clean -B build-release  # Clean variant build
```

### 3.5 pup distclean

```
pup distclean [OPTIONS]
```

Full reset: remove all generated files, the `.pup/` directory, and `tup.config`. Returns the project to a pristine state.

**Relevant Options:**
- `-n` - Dry-run: show what would be removed
- `-B DIR` - Distclean a variant build directory

**Examples:**
```bash
pup distclean          # Full reset of in-tree build
pup distclean -B build-debug  # Remove entire variant directory
```

### 3.6 pup variant

```
pup variant <config-file> [directory]
```

Create a variant (out-of-tree) build directory from a configuration file.

**Arguments:**
- `config-file` - Path to a tup.config file (e.g., `configs/release.config`)
- `directory` - Optional output directory name (default: derived from config filename)

**What it creates:**
- A new directory with symlinks to source Tupfiles
- A `tup.config` copied from the specified config file
- A `.pup/` directory for the variant's index

**Examples:**
```bash
pup variant configs/debug.config           # Creates build-debug/
pup variant configs/release.config out     # Creates out/
```

### 3.7 pup export

```
pup export <format> [OPTIONS]
```

Export build information in various formats.

**Formats:**
- `script` - Shell script
- `compdb` - compile_commands.json
- `graph` - DOT format dependency graph

#### 3.7.1 export script

```
pup export script > build.sh
```

Generate a shell script that runs all build commands in topological order. Useful for environments where pup isn't available or for debugging.

**Output:** Shell script to stdout

#### 3.7.2 export compdb

```
pup export compdb > compile_commands.json
```

Generate a [compilation database](https://clang.llvm.org/docs/JSONCompilationDatabase.html) for IDE integration. Works with clangd, ccls, and other tools.

**Output:** JSON array of compilation commands

**Example usage with clangd:**
```bash
pup export compdb > compile_commands.json
# IDE now has full code intelligence
```

#### 3.7.3 export graph

```
pup export graph [OPTIONS]
```

Export the dependency graph for visualization or analysis.

**Options:**
- `--summary` - Human-readable text output instead of DOT
- `-a, --all-deps` - Include implicit dependencies (headers from .d files)

**Examples:**
```bash
# Generate PNG visualization
pup export graph | dot -Tpng -o deps.png

# Text summary
pup export graph --summary

# Include header dependencies
pup export graph --all-deps | dot -Tsvg -o full-deps.svg
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
| `-B DIR` | | Build/output directory for variant builds. |
| `-A` | `--all` | Full project build, ignoring cwd scoping. |
| `-a` | `--all-deps` | Include implicit deps in graph output. |
| | `--stat` | Print build statistics after completion. |
| | `--summary` | Human-readable output (for `export graph`). |
| | `--version` | Print version information. |
| `-h` | `--help` | Print help message. |

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

Specify an out-of-tree build directory. All outputs and `.pup/` go here instead of the source tree.

```bash
pup -B build-release    # Build into build-release/
pup clean -B build-release  # Clean that variant
```

**`-A, --all` vs `-a, --all-deps`**

These are different options:
- `-A` / `--all` - Disable scoped builds; check all files regardless of cwd
- `-a` / `--all-deps` - Include implicit dependencies (headers) in graph export

### 4.2 Environment Variables

| Variable | Description |
|----------|-------------|
| `PUP_SOURCE_DIR` | Source directory (same as `-S`, lower priority) |
| `PUP_BUILD_DIR` | Build directory (same as `-B`, lower priority) |
| `PUP_IMPLICIT_DEPS` | Set to `0` to disable auto-generated dependency rules |

**Priority Order:**

For source/build directories:
1. Command-line options (`-S`, `-B`) - highest priority
2. Environment variables (`PUP_SOURCE_DIR`, `PUP_BUILD_DIR`)
3. Auto-detection from current working directory

**`PUP_IMPLICIT_DEPS`**

Controls automatic header dependency discovery via `gcc -M` rules.

```bash
# Disable implicit dependency generation
PUP_IMPLICIT_DEPS=0 pup build

# Enable (default)
PUP_IMPLICIT_DEPS=1 pup build
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
- `| order-only` - Dependencies that don't trigger rebuilds
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

# Order-only dependency (doesn't trigger rebuild)
: main.c | config.h |> gcc -c %f -o %o |> main.o

# Output to a group
: foreach *.c |> gcc -c %f -o %o |> %B.o {objs}

# Using a group as input
: {objs} |> gcc %f -o %o |> program
```

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

### 6.2 .pupignore / .tupignore

### 6.3 Tupfile.ini

## 7. Build Modes

### 7.1 In-Tree Builds

### 7.2 Variant Builds

### 7.3 Scoped Builds

## 8. Implicit Dependencies

### 8.1 Compiler .d Files

### 8.2 Auto-Generated Dependency Rules

## 9. Incremental Builds

### 9.1 Change Detection

### 9.2 The Index File

### 9.3 Stale Output Cleanup

## 10. Troubleshooting

### 10.1 Common Errors

### 10.2 Diagnostic Options

### 10.3 Debug Techniques

## Appendices

### A. Tup Compatibility Matrix

### B. Pattern Flags Reference

### C. Environment Variables Reference
