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
