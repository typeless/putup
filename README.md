# Pup

A modern C++20 reimplementation of the [Tup build system](https://gittup.org/tup/).

## Overview

Pup aims to be a drop-in replacement for tup with these design goals:

- **Compatibility** - Parse existing Tupfiles without modification
- **Modern C++20** - Clean codebase with minimal dependencies
- **Content hashing** - SHA-256 for precise change detection
- **Custom index** - Git-inspired binary format instead of SQLite
- **No FUSE** - Filesystem changes detected via index comparison
- **No Lua** - Traditional Tupfile syntax only

## Installation

### Building from Source

Pup uses tup to build itself:

```bash
git clone https://github.com/user/pup.git
cd pup
tup init
tup
```

Or use the Makefile wrapper:

```bash
make          # Build
make test     # Run tests
make install  # Install to /usr/local/bin
```

### Dependencies

- C++20 compiler (GCC 11+, Clang 14+)
- tup (for building)

Third-party libraries are vendored in `third_party/`:
- [expected-lite](https://github.com/martinmoene/expected-lite)
- [fmt](https://github.com/fmtlib/fmt)
- [Catch2](https://github.com/catchorg/Catch2) (tests only)

## Usage

```bash
pup [OPTIONS] [COMMAND]

Commands:
  init     Initialize .pup directory
  build    Execute build (default)
  parse    Parse and validate Tupfiles
  graph    Print dependency graph (graphviz format)
  clean    Remove generated files

Options:
  -j, --jobs N       Run N jobs in parallel (default: auto-detect)
  -k, --keep-going   Continue building after failures
  -n, --dry-run      Print commands without executing
  -v, --verbose      Show commands as they execute
  --variant=DIR      Use DIR as variant output directory
  --version          Print version information
  -h, --help         Show help
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

Out-of-tree builds via variant directories:

```bash
mkdir build && cd build
echo "CONFIG_DEBUG=y" > tup.config
pup --variant=.
```

## Tup Compatibility

### Fully Supported

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
| `.gitignore` generation | ✅ |
| Parallel execution | ✅ |
| Incremental builds | ✅ |
| Header dependency tracking | ✅ |

### Parsed but Not Evaluated

These directives are recognized by the parser but have no effect at build time:

| Feature | Notes |
|---------|-------|
| `import VAR[=default]` | Environment variable import not implemented |
| `export VAR` | Environment export not implemented |
| `run ./script` | Script execution for dynamic rules not implemented |
| `preload dir` | Subdirectory preloading not implemented |
| `error message` | Error directive not implemented |

### Not Implemented

| Feature | Notes |
|---------|-------|
| Bins `<name>` | Output bins (different from groups) |
| `tup monitor` | File watching (by design - no FUSE) |
| `tup scan` | Filesystem scanning |
| `tup variant` | Variant directory creation |
| `tup generate` | Makefile/script export |
| `tup compiledb` | compile_commands.json generation |
| `tup todo` | Show pending build steps |
| `tup varsed` | @-variable substitution in files |
| Lua Tupfiles | Lua scripting support |

### Behavioral Differences

| Aspect | Tup | Pup |
|--------|-----|-----|
| Database | SQLite | Custom binary index |
| Change detection | FUSE + mtime | mtime → size → SHA-256 |
| Directory | `.tup/` | `.pup/` |
| Implicit deps | FUSE interception | `.d` file parsing |

## Project Status

Pup is under active development. Current phase: **Polish**

- ✅ Foundation - Core types, hashing, error handling
- ✅ Parser - Lexer, AST, parser, evaluator
- ✅ Graph - Dependency DAG, topological sort
- ✅ Index - Binary format, reader/writer
- ✅ Execution - Scheduler, parallel builds, incremental
- 🔄 Polish - Edge cases, compatibility, performance

### Known Limitations

1. **No `import`/`export`** - Can't read environment variables or pass them to commands
2. **No `run` directive** - Dynamic rule generation from scripts not supported
3. **No bins** - The `<binname>` syntax for output bins is not implemented
4. **No FUSE** - Implicit dependencies only via `.d` files, not filesystem interception

### Roadmap

Priority features for full tup compatibility:

1. `import` / `export` - Environment variable handling
2. `compiledb` - IDE/LSP integration
3. Bins `<name>` - Output bin support
4. `run` directive - Dynamic rule generation
5. Target filtering - Build specific outputs

## Contributing

Contributions welcome! The codebase follows modern C++20 conventions:

- Almost Always Auto (AAA) with explicit type wrappers
- Trailing return types
- `Result<T>` for error handling (no exceptions)
- WebKit-based formatting

Run the test suite before submitting:

```bash
make check   # Format check + clang-tidy + tests
```

## License

MIT License - see [LICENSE](LICENSE) for details.

## Acknowledgments

- [Tup](https://gittup.org/tup/) by Mike Shal - the original build system
- Design inspired by Git's index format and content-addressable storage
