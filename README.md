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
  graph             Print dependency graph (graphviz format)
  clean             Remove generated files
  variant <config> [dir]  Create variant build directory

Options:
  -S, --source-dir DIR  Source directory (default: auto-detect)
  -B, --build-dir DIR   Build/output directory
  -j, --jobs N          Run N jobs in parallel (default: auto-detect)
  -k, --keep-going      Continue building after failures
  -n, --dry-run         Print commands without executing
  -v, --verbose         Show commands as they execute
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
pup graph | dot -Tpng > deps.png

# Create a variant build directory
pup variant default.config build
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

Pup automatically tracks header dependencies when using `-MD`:

```tup
CFLAGS += -MD
: foreach *.c |> $(CC) $(CFLAGS) -c %f -o %o |> %B.o
```

The compiler generates `.d` files listing included headers. Pup parses these and rebuilds affected objects when headers change.

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
| `tup generate` | Makefile/script export |
| `tup compiledb` | compile_commands.json generation |
| Lua Tupfiles | Lua scripting support |

### Behavioral Differences

| Aspect | Tup | Pup |
|--------|-----|-----|
| Change detection | FUSE + mtime | mtime → size → SHA-256 |
| Directory | `.tup/` | `.pup/` |
| Implicit deps | FUSE interception | `.d` file parsing |

## Known Limitations

1. **No `run` directive** - Dynamic rule generation from scripts not supported
2. **No bins** - The `<binname>` syntax for output bins is not implemented
3. **No FUSE** - Implicit dependencies only via `.d` files, not filesystem interception

## License

MIT License - see [LICENSE](LICENSE) for details.

## Acknowledgments

- [Tup](https://gittup.org/tup/) by Mike Shal - the original build system
