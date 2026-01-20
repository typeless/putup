# Putup

A build system using the [Tupfile](https://gittup.org/tup/) format.

> **Note:** The binary is named `putup`, but `pup` works as an alias.

- **Tupfile-based** - Uses Tup's Tupfile syntax for build rules
- **Content hashing** - SHA-256 for change detection
- **Scoped builds** - Limit builds to specific subdirectories
- **No FUSE** - Index-based tracking, works everywhere

## Installation

```bash
git clone https://github.com/user/pup.git
cd pup
make
```

Requirements: C++20 compiler (GCC 11+, Clang 14+)

### Bootstrapping

Putup is self-hosting (builds itself), but bootstrap scripts are provided for initial installation:

```bash
./bootstrap-linux.sh    # Linux
./bootstrap-macos.sh    # macOS
./bootstrap-win32.sh    # Windows (MSYS2/MinGW)
```

To regenerate bootstrap scripts after changes:
```bash
putup show script -B build > bootstrap-linux.sh
TUP_PLATFORM=macos putup show script -B build > bootstrap-macos.sh
TUP_PLATFORM=win32 putup show script -B build > bootstrap-win32.sh
```

## Quick Start

Create a simple project:

```bash
mkdir hello && cd hello
```

**Tupfile** - defines how to build:
```tup
: hello.c |> gcc %f -o %o |> hello
```

**hello.c** - your source:
```c
#include <stdio.h>
int main() { printf("Hello, world!\n"); return 0; }
```

Build it:
```bash
putup configure    # Set up build (creates tup.config)
putup              # Build
./hello            # Run it
```

Common commands:
```bash
putup -j8          # Build with 8 parallel jobs
putup -n           # Dry-run: show what would build
putup clean        # Remove generated files
```

## Concepts

### Variant Builds

A **variant** is a separate build configuration (debug, release, cross-compile, etc.) with its own output directory. Each variant has a `tup.config` file that defines configuration variables.

```bash
# Build specific variant
putup build-debug

# Build multiple variants in parallel
putup build-debug build-release

# Glob pattern for all variants
putup build-*
```

Variants keep build outputs isolated - you can switch between configurations without rebuilding from scratch.

### Configure Workflow

Putup uses a two-pass workflow: **configure** sets up the build environment, then **build** executes.

```bash
putup configure    # Pass 1: Create tup.config
putup              # Pass 2: Build
```

The `configure` command:
- Runs only rules that output `tup.config` files
- Creates an empty `tup.config` if no config rules exist
- Does NOT create the `.pup/` index (that happens on first build)

For variant builds, use `-B` to specify the output directory:

```bash
putup configure -B build-debug    # Creates build-debug/tup.config
putup build-debug                 # Build the variant
```

If you skip configure, build will error:
```
Error: No tup.config found. Run 'putup configure' first.
```

### Scoped Builds

A **scoped build** limits the build to a specific subdirectory. Only commands within that scope (and their dependencies) are executed.

```bash
# Build only src/lib and its deps
putup src/lib

# Combine with variant
putup build-debug/src/lib
```

Scoped builds are useful for large projects where you're working on a specific module. Use `-a` to include upstream dependencies, or `-A` to build the full project.

### Target Syntax

Targets are paths that select variants and scopes:

```
[variant/][scope]
```

Putup interprets a target as follows:

1. **Variant** - A directory containing `tup.config` (e.g., `build-debug/`)
2. **Scope** - A source subdirectory to limit the build (e.g., `src/lib`)
3. **Combined** - Variant prefix + scope (e.g., `build-debug/src/lib`)

```bash
putup build-debug           # Variant only: build everything in build-debug
putup src/lib               # Scope only: build src/lib across all variants
putup build-debug/src/lib   # Combined: build src/lib in build-debug variant
```

Glob patterns work for variant selection:

```bash
putup build-*               # All variants matching build-*
putup build-*/src/lib       # Scope src/lib in all matching variants
```

## CLI Reference

```
putup [OPTIONS] [TARGETS...]
putup [OPTIONS] <command> [TARGETS...]
```

### Commands

| Command | Description |
|---------|-------------|
| `clean` | Remove generated files |
| `configure` | Run only config-generating rules (outputs `tup.config`) |
| `distclean` | Full reset: remove `.pup` and variant directory |
| `parse` | Parse Tupfiles without building |
| `show <format>` | Show build info (see below) |

### Show Formats

| Format | Description |
|--------|-------------|
| `script` | Shell script to run build commands |
| `compdb` | `compile_commands.json` for IDE integration |
| `graph` | DOT format dependency graph |
| `graph --summary` | Human-readable text summary |

### Options

| Option | Description |
|--------|-------------|
| `-j, --jobs N` | Run N jobs in parallel |
| `-k, --keep-going` | Continue after failures |
| `-n, --dry-run` | Print commands without executing |
| `-v, --verbose` | Verbose output |
| `-S DIR` | Source directory (default: auto-detect) |
| `-B DIR` | Build/output directory (can repeat) |
| `-A, --all` | Full project build (ignore cwd scoping) |
| `-a, --all-deps` | Include upstream deps in scoped builds |
| `--stat` | Print build statistics |
| `--summary` | Human-readable output (for `putup show graph`) |

### Examples

```bash
# Basic builds
putup                      # Build all variants
putup build-debug          # Build single variant
putup build-*              # Build all matching variants

# Scoped builds
putup src/lib              # Build only src/lib across all variants
putup build-debug/src/lib  # Build src/lib in specific variant

# Show build info
putup show compdb          # Generate compile_commands.json
putup show graph --summary # Show dependency stats
```

### Environment Variables

| Variable | Description |
|----------|-------------|
| `PUP_SOURCE_DIR` | Source directory (overridden by `-S`) |
| `PUP_BUILD_DIR` | Build directory (overridden by `-B`) |
| `PUP_IMPLICIT_DEPS` | Set to `0` to disable auto-generated dep rules |

## Documentation

- **[Reference Manual](docs/reference.md)** - Complete command reference, Tupfile syntax, configuration, and troubleshooting
- **[CLAUDE.md](CLAUDE.md)** - Developer guide: code style, testing, architecture

## License

MIT License - see [LICENSE](LICENSE) for details.
