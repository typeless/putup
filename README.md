# Pup

A modern reimplementation of the [Tup build system](https://gittup.org/tup/).

## Overview

Pup aims to be a drop-in replacement for tup:

- **Compatibility** - Parse existing Tupfiles without modification
- **Content hashing** - SHA-256 for precise change detection
- **No FUSE required** - Filesystem changes detected via index comparison
- **No Lua** - Traditional Tupfile syntax only

## Installation

### Building from Source

Pup uses tup to build itself:

```bash
git clone https://github.com/user/pup.git
cd pup
pup variant default.config build
tup
```

Or use the Makefile wrapper:

```bash
make          # Build
make test     # Run tests
make install  # Install to /usr/local/bin
```

### Dependencies

Build requirements:
- C++20 compiler (GCC 11+, Clang 14+)
- tup (for bootstrapping)

## Usage

```bash
pup [OPTIONS] [COMMAND]

Commands:
  init              Initialize .pup directory
  build             Execute build (default)
  parse             Parse and validate Tupfiles
  export <format>   Export build info (script, compdb, graph)
  clean             Remove generated files
  distclean         Full reset: remove .pup and variant directory
  variant <config> [dir]  Create variant build directory

Options:
  -S, --source-dir DIR  Source directory (default: auto-detect)
  -B, --build-dir DIR   Build/output directory
  -j, --jobs N          Run N jobs in parallel (default: auto-detect)
  -k, --keep-going      Continue building after failures
  -n, --dry-run         Print commands without executing
  -v, --verbose         Show commands as they execute
  --summary             Human-readable output (for export graph)
  --version             Print version information
  -h, --help            Show help
```

### Quick Start

```bash
# Initialize a new project
pup init

# Build the project
pup

# Build with 8 parallel jobs
pup -j8

# See what would be built
pup -n

# Generate dependency graph
pup export graph | dot -Tpng > deps.png

# Generate compile_commands.json for clangd
pup export compdb > compile_commands.json

# Generate a standalone build script
pup export script > build.sh

# Create a variant build directory
pup variant default.config build

# Full reset (remove .pup and variant directory)
pup distclean -B build
```

### Tupfile Syntax

Pup supports standard Tupfile syntax:

```tup
# Variables
CC = gcc
CFLAGS = -Wall -O2

# Rules
: foreach *.c |> $(CC) $(CFLAGS) -c %f -o %o |> %B.o {objs}
: {objs} |> $(CC) -o %o %f |> myprogram

# Bang macros
!cc = |> ^ CC %o^ $(CC) $(CFLAGS) -c %f -o %o |> %B.o
: foreach *.c |> !cc |> {objs}

# Conditionals
ifdef DEBUG
  CFLAGS += -g
endif

# Environment variables
import CC
export PKG_CONFIG_PATH

# Include shared rules
include_rules
```

### Implicit Header Dependencies

Pup offers two methods for tracking header dependencies:

#### Method 1: Compiler-generated `.d` files (recommended)

Add `-MD` to your compile flags:

```tup
CFLAGS += -MD
: foreach *.c |> $(CC) $(CFLAGS) -c %f -o %o |> %B.o
```

The compiler generates `.d` files listing included headers. Pup parses these and rebuilds affected objects when headers change.

#### Method 2: Auto-generated dependency scanning

Set `PUP_IMPLICIT_DEPS=1` to have pup automatically generate dependency scanning rules:

```bash
PUP_IMPLICIT_DEPS=1 pup build
```

When enabled, pup pattern-matches C/C++ compile commands and auto-generates `gcc -M` rules to discover header dependencies. This works without modifying your Tupfiles:

```tup
# Your Tupfile (no -MD needed):
: main.c |> gcc -c %f -o %o |> main.o

# Pup auto-generates:
# : main.c |> gcc -M main.c |> <stdout → implicit deps for main.o>
```

The discovered headers become implicit edges in the dependency graph, triggering rebuilds when headers change.

### Variant Builds

Create variant builds for different configurations:

```bash
# Create a config file
echo "CONFIG_DEBUG=y" > debug.config

# Create variant directory with symlinked config
pup variant debug.config build-debug

# Build in the variant directory
cd build-debug && pup
```

Or use separate source and build directories:

```bash
pup -S /path/to/source -B /path/to/build
```

## Tup Compatibility

### Supported

| Feature | Status |
|---------|--------|
| Rules (`: \|> \|>`) | ✅ |
| Variables `$(VAR)` | ✅ |
| Config variables `@(VAR)` | ✅ |
| Node variables `&(VAR)` | ✅ |
| Bang macros `!name` | ✅ |
| Groups `{name}` | ✅ |
| Order-only deps `\|` | ✅ |
| Pattern flags (`%f`, `%o`, `%B`, etc.) | ✅ |
| Display text `^ text ^` | ✅ |
| Conditionals (`ifdef`, `ifeq`, etc.) | ✅ |
| `include` / `include_rules` | ✅ |
| `import` / `export` | ✅ |
| `.gitignore` generation | ✅ |
| Parallel execution | ✅ |
| Incremental builds | ✅ |
| Header dependency tracking | ✅ |
| Variant builds | ✅ |

### Not Implemented

| Feature | Notes |
|---------|-------|
| `run ./script` | Dynamic rule generation from scripts |
| `preload dir` | Subdirectory preloading |
| Bins `<name>` | Output bins (different from groups) |
| `tup monitor` | File watching (no FUSE by design) |
| Lua Tupfiles | Lua scripting support |

### Behavioral Differences

| Aspect | Tup | Pup |
|--------|-----|-----|
| Change detection | FUSE + mtime | mtime → size → SHA-256 |
| Directory | `.tup/` | `.pup/` |
| Implicit deps | FUSE interception | `.d` files or auto-generated `gcc -M` |

## Known Limitations

1. **No `run` directive** - Dynamic rule generation from scripts not supported
2. **No bins** - The `<binname>` syntax for output bins is not implemented
3. **No FUSE** - Implicit dependencies only via `.d` files, not filesystem interception

## License

MIT License - see [LICENSE](LICENSE) for details.

## Acknowledgments

- [Tup](https://gittup.org/tup/) by Mike Shal - the original build system
