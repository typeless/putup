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

## Documentation

- **[Reference Manual](docs/reference.md)** - Complete user guide: commands, Tupfile syntax, configuration
- **[Compatibility](COMPATIBILITY.md)** - Tup compatibility matrix and migration guide
- **[CLAUDE.md](CLAUDE.md)** - Developer guide: building, testing, project structure
- **[STYLE.md](STYLE.md)** - C++ code style guide
- **[DESIGN.md](DESIGN.md)** - Internal architecture and design rationale

## License

MIT License - see [LICENSE](LICENSE) for details.
