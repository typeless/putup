# Pup

A modern reimplementation of the [Tup build system](https://gittup.org/tup/).

- **Drop-in compatible** - Parse existing Tupfiles without modification
- **Content-based** - SHA-256 hashing for precise change detection
- **No FUSE required** - Index-based tracking instead of filesystem interception
- **Scalable** - Scoped builds limit scanning to subtrees for fast incremental builds

Pup takes a different approach than tup's "beta build system" (FUSE + SQLite). Instead of monitoring all filesystem access, pup uses an index-based architecture with scoped scanning - trading automatic dependency detection for portability and predictable performance on large projects.

## Installation

```bash
git clone https://github.com/user/pup.git
cd pup
make
```

Requirements: C++20 compiler (GCC 11+, Clang 14+)

## Quick Start

```bash
pup              # Build the project (auto-detects variants)
pup -j8          # Build with 8 parallel jobs
pup -n           # Dry-run: show what would build
pup clean        # Remove generated files

# Multi-variant builds (auto-detected or explicit)
pup                          # Auto-build all variants in parallel
pup -B build-debug           # Build single variant
pup -B build-debug -B build-release  # Build specific variants
```

## Documentation

- **[Reference Manual](docs/reference.md)** - Complete command reference, Tupfile syntax, configuration, and troubleshooting
- **[CLAUDE.md](CLAUDE.md)** - Developer guide: code style, testing, architecture

## License

MIT License - see [LICENSE](LICENSE) for details.
