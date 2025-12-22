# Pup

A build system using the [Tupfile](https://gittup.org/tup/) format.

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
pup configure    # Set up build (creates tup.config)
pup              # Build
./hello          # Run it
```

Common commands:
```bash
pup -j8          # Build with 8 parallel jobs
pup -n           # Dry-run: show what would build
pup clean        # Remove generated files
```

## Concepts

### Variant Builds

A **variant** is a separate build configuration (debug, release, cross-compile, etc.) with its own output directory. Each variant has a `tup.config` file that defines configuration variables.

```bash
# Build specific variant
pup build-debug

# Build multiple variants in parallel
pup build-debug build-release

# Glob pattern for all variants
pup build-*
```

Variants keep build outputs isolated - you can switch between configurations without rebuilding from scratch.

### Configure Workflow

Pup uses a two-pass workflow: **configure** sets up the build environment, then **build** executes.

```bash
pup configure    # Pass 1: Create tup.config
pup              # Pass 2: Build
```

The `configure` command:
- Runs only rules that output `tup.config` files
- Creates an empty `tup.config` if no config rules exist
- Does NOT create the `.pup/` index (that happens on first build)

For variant builds, use `-B` to specify the output directory:

```bash
pup configure -B build-debug    # Creates build-debug/tup.config
pup build-debug                 # Build the variant
```

If you skip configure, build will error:
```
Error: No tup.config found. Run 'pup configure' first.
```

### Scoped Builds

A **scoped build** limits the build to a specific subdirectory. Only commands within that scope (and their dependencies) are executed.

```bash
# Build only src/lib and its deps
pup src/lib

# Combine with variant
pup build-debug/src/lib
```

Scoped builds are useful for large projects where you're working on a specific module. Use `-a` to include upstream dependencies, or `-A` to build the full project.

### Target Syntax

Targets are paths that select variants and scopes:

```
[variant/][scope]
```

Pup interprets a target as follows:

1. **Variant** - A directory containing `tup.config` (e.g., `build-debug/`)
2. **Scope** - A source subdirectory to limit the build (e.g., `src/lib`)
3. **Combined** - Variant prefix + scope (e.g., `build-debug/src/lib`)

```bash
pup build-debug           # Variant only: build everything in build-debug
pup src/lib               # Scope only: build src/lib across all variants
pup build-debug/src/lib   # Combined: build src/lib in build-debug variant
```

Glob patterns work for variant selection:

```bash
pup build-*               # All variants matching build-*
pup build-*/src/lib       # Scope src/lib in all matching variants
```

## CLI Reference

```
pup [OPTIONS] [TARGETS...]
pup [OPTIONS] <command> [TARGETS...]
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
| `--summary` | Human-readable output (for `show graph`) |

### Examples

```bash
# Basic builds
pup                      # Build all variants
pup build-debug          # Build single variant
pup build-*              # Build all matching variants

# Scoped builds
pup src/lib              # Build only src/lib across all variants
pup build-debug/src/lib  # Build src/lib in specific variant

# Show build info
pup show compdb          # Generate compile_commands.json
pup show graph --summary # Show dependency stats
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
