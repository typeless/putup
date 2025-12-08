# Pup - Tup Build System Reimplementation

A modern C++20 reimplementation of the [Tup build system](https://gittup.org/tup/).

## Project Goals

1. **Compatibility** - Parse existing Tupfile/Tuprules.tup with zero modifications
2. **Modern C++20** - Minimal third-party dependencies (expected-lite, fmt, Catch2)
3. **Git-inspired index** - Custom binary format instead of SQLite
4. **Content hashing** - SHA-256 for precise change detection
5. **No FUSE** - Compute changes from index comparison
6. **No Lua** - Traditional Tupfile syntax only

## Building & Testing

A Makefile wraps tup for common workflows:

```bash
make              # Build (quiet mode)
make V=1          # Build with verbose output
make test         # Run unit tests + E2E tests
make tidy         # Run clang-tidy
make format       # Format with clang-format
make check        # Full CI: format-check + tidy + test
make clean        # Clean and reinitialize tup
```

Or use tup directly:

```bash
tup init          # First time only
tup               # Build
./build/pup       # Run
```

Build artifacts go to `build/` using tup's variant system.

## Testing

```bash
make test                          # Run all tests
./build/test/unit/pup_test         # Unit tests only
./build/test/unit/pup_test -s      # Unit tests with verbose output
./test/e2e/run_tests.sh            # E2E tests only
```

## Code Style

### AAA (Almost Always Auto)
Use `auto` with explicit type initialization for all declarations:
```cpp
// Literals - type is clear from value
auto x = 42;
auto pi = 3.14;
auto name = "hello";

// Function calls - wrap return value in explicit type
auto result = Result{compute()};     // Not: auto result = compute();
auto node = Node{get_node()};        // Not: auto node = get_node();
auto count = std::size_t{vec.size()};

// Factory functions that return the type are OK as-is
auto ptr = std::make_unique<Foo>();  // make_unique<Foo> is explicit
auto opt = std::make_optional(42);   // make_optional is explicit

// References and pointers
auto const& ref = container;
auto* ptr = get_pointer();

// Explicit type when not obvious from RHS
auto result = std::string{};         // Empty init
auto vec = std::vector<int>{1,2,3};  // Initializer list
```

**Rationale**: The explicit type wrapper makes the type visible at the declaration site, improving readability while maintaining the benefits of `auto` (no redundant type on LHS).

### Trailing Return Types
```cpp
auto foo() -> ReturnType;
auto bar(int x) -> std::expected<Result, Error>;
```

### Right-side Const
```cpp
auto const& ref = value;   // Not: const auto& ref
int const* ptr;            // Not: const int* ptr
```

### Internal Linkage
Use anonymous namespaces instead of `static` for internal linkage:
```cpp
// Good - anonymous namespace
namespace {
auto helper_function() -> void { ... }
auto const MAGIC_VALUE = 42;
}

// Avoid - static for internal linkage
static auto helper_function() -> void { ... }  // Don't do this
static auto const MAGIC_VALUE = 42;            // Don't do this
```

**Exception**: `static` is fine for:
- `static constexpr` compile-time constants inside functions
- `static` class/struct member functions

### Other Guidelines
- WebKit-based formatting (see `.clang-format`)
- No braces required around single statements
- Namespace contents not indented
- Avoid unnecessary comments - code should be self-documenting

## Project Structure

```
pup/
├── build/              # Build output (tup variant directory)
│   ├── tup.config      # Variant configuration
│   ├── pup             # Main binary
│   └── test/unit/pup_test
├── include/pup/
│   ├── core/           # Core types, hash, result, platform
│   ├── parser/         # Lexer, parser, AST, evaluator, depfile
│   ├── graph/          # Dependency DAG, builder, topological sort
│   ├── index/          # Binary index format, reader/writer
│   └── exec/           # Scheduler, command runner
├── src/                # Implementation files
├── test/
│   ├── unit/           # Catch2 unit tests
│   └── e2e/            # End-to-end tests
│       ├── run_tests.sh
│       └── fixtures/   # Test fixtures (simple_c, multi_file, etc.)
├── third_party/        # expected-lite, fmt, sha256, Catch2
├── Makefile            # Workflow wrapper (make test, make tidy, etc.)
├── Tupfile             # Build configuration
└── Tuprules.tup        # Shared build rules
```

## Reference Projects

- `/home/mural/src/tup/` - Original tup source (C)
- `/home/mural/src/castlestech.com/megahunt/PPC_Linux/spos/` - Real-world tup usage
- `/home/mural/src/castlestech.com/megahunt/PPC_Linux/ctos/` - Multi-directory tup project (ARM cross-compile)

## Testing with ctos (Multi-Directory Project)

The ctos project is a real-world multi-directory tup project for ARM cross-compilation.
Use this to test multi-directory support once implemented.

### Prerequisites

```bash
# Source the Yocto SDK environment (sets CC, CXX, CFLAGS, etc.)
source ~/src/castlestech.com/megahunt/sdk/environment-setup-cortexa5t2hf-neon-oe-linux-gnueabi
```

### Project Structure

```
ctos/
├── Tupfile.ini          # Project root marker (empty)
├── .tup/                # Tup database
├── Tuprules.tup         # Shared rules with import/export
├── build-ppc/           # Variant output directory
│   └── tup.config       # Variant configuration
└── system/              # Subdirectories with Tupfiles
    ├── powerd/Tupfile
    ├── sensord/Tupfile
    └── ...
```

### Key Features Used

```tup
# Tuprules.tup imports from SDK environment
import TARGET_PREFIX
import CC
import CXX
import CFLAGS
import LDFLAGS

# Exports for pkg-config subprocess calls
export PKG_CONFIG_SYSROOT_DIR
export PKG_CONFIG_PATH

# Bang macros for cross-compilation
!cc = |> ^ CC %o^ $(CC) $(CFLAGS) -c -o %o %f |>
```

### Testing Commands

```bash
# Once multi-directory support is implemented:
cd ~/src/castlestech.com/megahunt/PPC_Linux/ctos
~/src/pup/build/pup build

# Compare with tup:
tup
```

### Current Status

- ❌ Multi-directory Tupfile scanning not implemented
- ✅ `import` directive implemented
- ✅ `export` directive implemented
- ✅ Bang macros implemented

## Tupfile Syntax Features to Support

### Rules
```tup
: [foreach] inputs [| order-only] |> command |> outputs [{group}]
```

### Variables
```tup
VAR = value           # Assignment
VAR += value          # Append
VAR := value          # No expansion
$(VAR)                # Reference
@(CONFIG_VAR)         # Config variable
&(NODE_VAR)           # Node variable
```

### Bang-Macros
```tup
!cc = |> ^ CC %o^ $(CC) -c %f -o %o |> %B.o
: foreach *.c |> !cc |> {objs}
```

### Pattern Flags
- `%f` - All inputs
- `%b` - Basename with extension
- `%B` - Basename without extension
- `%e` - Extension (foreach only)
- `%o` - All outputs
- `%d` - Directory name
- `%g` - Glob match portion
- `%1f`, `%2f` - Nth input

### Conditionals
```tup
ifdef VAR / ifndef VAR
ifeq ($(VAR),value) / ifneq ($(VAR),value)
else
endif
```

### Directives
- `include_rules` - Include Tuprules.tup
- `include path` - Include specific file
- `export VAR` - Export environment variable
- `import VAR[=default]` - Import config variable

## Implicit Header Dependencies

Pup automatically tracks header dependencies by parsing `.d` files generated by the compiler.

### Usage
Add `-MD` to your compile flags:
```tup
CFLAGS += -MD
: foreach *.c |> $(CC) $(CFLAGS) -c %f -o %o |> %B.o
```

### How It Works
1. Compiler generates `foo.d` alongside `foo.o` listing all included headers
2. After successful compilation, pup parses the `.d` file
3. Discovered headers stored as `LinkType::Implicit` edges in `.pup/index`
4. On subsequent builds, changed headers trigger rebuild of dependent commands
5. **All headers tracked** including system headers (`/usr/include/*`)

### Change Detection
When determining if a file changed:
1. Compare mtime - if different, rebuild
2. If mtime same, compare file size - if different, rebuild
3. If size same, compute SHA-256 hash - if different, rebuild

### .d File Format
```makefile
foo.o: foo.c \
  include/header.h \
  /usr/include/stdio.h
```

The depfile parser (`include/pup/parser/depfile.hpp`) handles:
- Backslash line continuations
- Escaped spaces in paths
- Windows (CRLF) line endings

## Index Format

Binary file at `.pup/index`:
- Header (64 bytes): magic, version, counts
- File entries (96 bytes each): id, path, mtime, size, SHA-256 hash
- Command entries (64 bytes each): id, command, display
- Edges (24 bytes each): from, to, type (Normal, Sticky, Group, Implicit)
- String table: packed strings
- Footer (32 bytes): SHA-256 checksum

## Implementation Phases

1. ✅ **Foundation** - Core types, hash, result, platform
2. ✅ **Parser** - Lexer, AST, parser, evaluator, depfile
3. ✅ **Graph** - DAG, builder, topological sort
4. ✅ **Index** - Binary format, reader/writer, implicit deps
5. ✅ **Execution** - Scheduler, command runner, incremental builds
6. 🔄 **Polish** - Edge cases, error handling, performance
