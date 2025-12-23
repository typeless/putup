---
name: pup
description: Set up pup-based build systems. Migrate projects from Make/CMake/tup to pup.
---

# Pup Build System

Build system using Tupfile syntax with content-based incremental builds.

## When to Invoke Me

- Setting up new pup projects
- Migrating from Make/CMake/tup
- Writing Tupfiles and Tuprules.tup
- Configuring variant builds (debug/release/cross-compile)
- Troubleshooting pup build issues

## Project Structure

### Minimum Required Files

```
project/
├── Tupfile.ini        # Project root marker (can be empty)
├── Tupfile            # Build rules
└── tup.config         # Configuration (created by pup configure)
```

### Recommended Structure

```
project/
├── Tupfile.ini
├── Tuprules.tup       # Shared rules and bang macros
├── Tupfile            # Build rules
├── configs/           # Config files (for multi-config projects)
│   ├── linux.config
│   ├── macos.config
│   └── win32.config
└── src/
    └── Tupfile        # Subdirectory rules
```

### Variant Build Layout

```
project/
├── Tupfile.ini
├── src/
├── build-debug/       # Variant directory
│   ├── tup.config     # Variant config
│   └── .pup/          # Build index
└── build-release/
    └── tup.config
```

## Tupfile Syntax Quick Reference

### Rules

```tup
: inputs |> command |> outputs

# Foreach: one command per input
: foreach *.c |> gcc -c %f -o %o |> %B.o

# Order-only dependency (doesn't trigger rebuild)
: main.c | generated.h |> gcc -c %f -o %o |> main.o

# Output to group
: foreach *.c |> !cc |> %B.o {objs}

# Use group as input
: {objs} |> !link |> program
```

### Pattern Flags

| Flag | Description | Example |
|------|-------------|---------|
| `%f` | All inputs | `foo.c bar.c` |
| `%o` | All outputs | `foo.o` |
| `%B` | Basename without extension | `foo` |
| `%b` | Basename with extension | `foo.c` |
| `%e` | Extension only | `c` |
| `%d` | Directory | `src` |

### Variables

```tup
CC = gcc
CFLAGS = -Wall -O2
CFLAGS += -g              # Append

$(CC)                     # Regular variable
@(CONFIG_VAR)             # From tup.config (reads CONFIG_CONFIG_VAR)
$(TUP_CWD)                # Built-in: current directory
$(TUP_PLATFORM)           # Built-in: linux, macosx, win32
```

### Bang Macros

```tup
# Definition
!cc = |> ^ CC %f^ $(CC) $(CFLAGS) -c %f -o %o |> %B.o

# Usage
: foreach *.c |> !cc |> {objs}
```

### Directives

```tup
include_rules             # Include Tuprules.tup from current/parent dirs
include config.tup        # Include specific file
import CC                 # Import from environment/config
export PATH               # Export to command environment
```

## Tuprules.tup Template

```tup
ROOT = $(TUP_CWD)

# OS API (posix or win32)
OS_API = @(OS_API)
OS_API ?= posix

# Toolchain (CROSS_COMPILE convention from Linux kernel)
import CROSS_COMPILE=
import CC=$(CROSS_COMPILE)gcc
import CXX=$(CROSS_COMPILE)g++
import AR=$(CROSS_COMPILE)ar

# Base flags
CFLAGS = -std=c11 -Wall -Wextra -Werror
CXXFLAGS = -std=c++20 -Wall -Wextra -Werror -I$(ROOT)/include

# Build mode flags (set in tup.config)
CFLAGS += @(DEBUG_CFLAGS) @(RELEASE_CFLAGS)
CXXFLAGS += @(DEBUG_CXXFLAGS) @(RELEASE_CXXFLAGS)
LDFLAGS += @(DEBUG_LDFLAGS) @(RELEASE_LDFLAGS) @(PLATFORM_LDFLAGS)

# Bang macros
!cc = |> ^ CC %f^ $(CC) $(CFLAGS) -c %f -o %o |> %B.o
!cxx = |> ^ CXX %f^ $(CXX) $(CXXFLAGS) -c %f -o %o |> %B.o
!link = |> ^ LINK %o^ $(CXX) %f -o %o $(LDFLAGS) |>
!ar = |> ^ AR %o^ $(AR) rcs %o %f |>
```

## Config Files

### Format

```ini
# tup.config - Variables must start with CONFIG_
CONFIG_OS_API=posix
CONFIG_DEBUG_CFLAGS=-g -O0
CONFIG_RELEASE_CFLAGS=-O2 -DNDEBUG
```

### Example: linux.config

```ini
CONFIG_OS_API=posix
CONFIG_RELEASE_CFLAGS=-O2 -ffunction-sections -fdata-sections
CONFIG_RELEASE_CXXFLAGS=-O2 -DNDEBUG -ffunction-sections -fdata-sections
CONFIG_RELEASE_LDFLAGS=-Wl,--gc-sections
```

### Example: macos.config

```ini
CONFIG_OS_API=posix
CONFIG_RELEASE_CFLAGS=-O2 -ffunction-sections -fdata-sections
CONFIG_RELEASE_CXXFLAGS=-O2 -DNDEBUG -ffunction-sections -fdata-sections
CONFIG_RELEASE_LDFLAGS=-Wl,-dead_strip
```

### Example: win32.config

```ini
CONFIG_OS_API=win32
CONFIG_RELEASE_CFLAGS=-O2 -DNDEBUG
CONFIG_RELEASE_CXXFLAGS=-O2 -DNDEBUG
CONFIG_PLATFORM_LDFLAGS=-static
```

### Simple Config Generation (in Tupfile)

```tup
# For simple projects, put config rules in a regular Tupfile
: $(TUP_PLATFORM).config |> cp %f %o |> ../tup.config
```

### Complex Config Generation (dedicated configs/)

```tup
# configs/Tupfile
: $(TUP_PLATFORM).config |> cp %f %o |> ../tup.config
```

## Common Patterns

### Generated Headers with Order-Only Groups

For projects with generated headers, use order-only groups to ensure headers exist before compilation:

```tup
# include/Tupfile - Generate headers
: ../.config |> ^ GEN autoconf.h^ ./gen_autoconf.sh %f %o |> autoconf.h <gen-headers>
: ../.config |> ^ GEN config.h^ ./gen_config.sh %f %o |> config.h <gen-headers>

# src/Tupfile - Compile with dependency on headers
: foreach *.c | ../include/<gen-headers> |> !cc |> {objs}
```

For chained dependencies (one generated file needs another):

```tup
# Generate applets.h first
: ../.config |> ^ GEN applets.h^ ./gen_applets.sh |> applets.h <gen-headers>

# embedded_scripts.h needs applets.h (use order-only dep)
: ../.config | applets.h autoconf.h |> ^ GEN embedded.h^ ./gen_embedded.sh |> embedded.h <gen-headers>
```

### Running Scripts from Subdirectory Tupfiles

When a Tupfile in a subdirectory needs to run scripts from project root:

```tup
# include/Tupfile running scripts from ../scripts/
: ../.config |> ^ GEN header.h^ cd .. && ./scripts/gen_header.sh include/header.h |> header.h
```

For scripts needing environment variables:

```tup
: ../.config |> ^ GEN header.h^ cd .. && srctree=. HOSTCC=gcc ./scripts/gen_header.sh |> header.h
```

### Extracting Source Lists from Existing Builds

When migrating, extract the actual source list from a working build:

```bash
# From Make builds
make V=1 2>&1 | grep -oE '[a-zA-Z0-9_/]+\.c' | sort -u > sources.txt

# Generate Tupfile rules
while read src; do
  dir=$(dirname "$src")
  echo ": $src | include/<gen-headers> |> ^ CC %f^ \$(CC) \$(CFLAGS) -c %f -o %o |> build/$dir/$(basename "$src" .c).o {objs}"
done < sources.txt
```

### Makefile.pup Wrapper

Provide a familiar make interface for projects migrated to pup:

```makefile
# Makefile.pup
.PHONY: all build clean distclean

all: build

build: .config
	pup configure -B build
	pup build -j$$(nproc)

clean:
	pup clean

distclean:
	pup distclean
	rm -f .config include/*.h  # Clean generated files

.config:
	@echo "Run 'make defconfig' first" && exit 1
```

Usage: `make -f Makefile.pup`

### Explicit Source Lists (srcs-y pattern)

Prefer explicit lists over globs for visibility and control:

```tup
srcs-y  = src/main.cpp
srcs-y += src/parser.cpp
srcs-y += src/lexer.cpp

: foreach $(srcs-y) |> !cxx |> {objs}
```

### Platform Selection via Filenames

Use `foo-@(VAR)` patterns instead of `ifeq/ifneq`:

```tup
# Instead of conditionals, use variable substitution in filenames
srcs-y += src/platform-$(OS_API).cpp

# File structure:
#   src/platform-posix.cpp
#   src/platform-win32.cpp
```

### Conditional Compilation (foo-y pattern)

```tup
# Enable features via config
srcs-y += src/core.cpp
srcs-@(ENABLE_DEBUG) += src/debug.cpp    # Only if CONFIG_ENABLE_DEBUG=y
```

### Implicit Dependencies

Header dependencies are detected automatically by pup. No special flags needed.

### Ignoring Files

Create `.pupignore` (or `.tupignore`) at project root:

```gitignore
*.o
build/
.vscode/
```

## Commands Reference

### Essential Commands

```bash
pup configure -B build     # Create variant, generate tup.config
pup                        # Build (auto-detects variants)
pup build-debug            # Build specific variant
pup clean                  # Remove generated files
pup distclean              # Full reset (removes .pup/ and tup.config)
pup parse                  # Validate Tupfiles without building
```

### Diagnostic Commands

```bash
pup show script            # Generate build.sh
pup show compdb            # Generate compile_commands.json
pup show graph             # DOT format dependency graph
pup show graph --summary   # Human-readable summary
```

### Common Options

| Option | Description |
|--------|-------------|
| `-j N` | Parallel jobs (default: CPU count) |
| `-n` | Dry-run: show commands without executing |
| `-v` | Verbose output |
| `-k` | Continue after failures |
| `-B DIR` | Build directory (variant) |

### Environment Variables

| Variable | Description |
|----------|-------------|
| `CROSS_COMPILE` | Toolchain prefix (e.g., `arm-none-eabi-`) |
| `PUP_IMPLICIT_DEPS` | Set to `0` to disable auto header tracking |

## Migration Checklist

### Phase 1: Setup

- [ ] Create `Tupfile.ini` at project root (can be empty)
- [ ] Create `Tuprules.tup` with toolchain and bang macros
- [ ] Create config files (or single `tup.config`)

### Phase 2: Build Rules

- [ ] Write root `Tupfile` with source lists
- [ ] Add `Tupfile` to each source directory (or use paths from root)
- [ ] Use `include_rules` at top of each Tupfile
- [ ] Replace Make variables with Tupfile equivalents

### Phase 3: Configuration

- [ ] Create `configs/` directory (if multiple configs needed)
- [ ] Add platform-specific configs (linux.config, macos.config, etc.)
- [ ] Write config generation rule in Tupfile

### Phase 4: Testing

- [ ] Run `pup parse` to validate syntax
- [ ] Run `pup -n` to see commands without executing
- [ ] Run `pup` to build
- [ ] Run `pup show compdb > compile_commands.json` for IDE integration
- [ ] Verify incremental builds work (change a file, rebuild)

## Real-World Example: Busybox

See `examples/busybox/` for a complete migration of Busybox (582 source files, 6 generated headers):

```bash
# Download busybox
wget https://busybox.net/downloads/busybox-1.38.0.tar.bz2
tar xjf busybox-1.38.0.tar.bz2 && cd busybox-1.38.0

# Overlay pup build files
rsync -av /path/to/pup/examples/busybox/ ./

# Build
make -f Makefile.pup defconfig
make -f Makefile.pup
```

Key patterns demonstrated:
- Order-only `<gen-headers>` group for 6 generated headers
- Chained header generation (embedded_scripts.h depends on applets.h)
- Running kconfig tools from include/Tupfile
- Makefile.pup wrapper for familiar interface

## Quick Start Example

```bash
# 1. Create project structure
mkdir myproject && cd myproject
touch Tupfile.ini

# 2. Create Tuprules.tup (copy template above)

# 3. Create Tupfile
cat > Tupfile << 'EOF'
include_rules
srcs-y = main.cpp
: foreach $(srcs-y) |> !cxx |> {objs}
: {objs} |> !link |> myapp
EOF

# 4. Create tup.config
cat > tup.config << 'EOF'
CONFIG_OS_API=posix
CONFIG_RELEASE_CFLAGS=-O2
CONFIG_RELEASE_CXXFLAGS=-O2
EOF

# 5. Build
pup configure && pup
```
