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

## Commands

```bash
pup [OPTIONS] [COMMAND]
```

| Command | Description |
|---------|-------------|
| `init` | Initialize `.pup` directory in current project |
| `parse` | Parse and validate Tupfiles without building |
| `build` | Execute build (default if no command specified) |
| `export <format>` | Export build info (see below) |
| `clean` | Remove generated output files (using index) |
| `distclean` | Full reset: remove outputs, `.pup`, and `tup.config` |
| `variant <config> [dir]` | Create variant build directory from config file |

### Export Formats

| Format | Description |
|--------|-------------|
| `script` | Shell script that runs all commands in order |
| `compdb` | compile_commands.json for IDE integration |
| `graph` | DOT format for graphviz (add `--summary` for text output) |

### Common Options

| Option | Description |
|--------|-------------|
| `-j N` | Run N jobs in parallel |
| `-k` | Keep going after failures |
| `-n` | Dry-run: print commands without executing |
| `-v` | Verbose output |
| `-S DIR` | Source directory (default: auto-detect) |
| `-B DIR` | Build/output directory for variant builds |
| `--summary` | Human-readable output (for `export graph`) |

### Examples

```bash
# Basic build
pup build

# Variant build (out-of-tree)
pup build -B build-release

# Clean generated outputs (keeps .pup and tup.config)
pup clean -B build-release

# Full reset of variant build
pup distclean -B build-release

# In-tree builds (auto-detected via tup.config or .pup at root)
pup clean       # Remove outputs
pup distclean   # Remove outputs + .pup + tup.config

# Dry-run to see what would be removed
pup clean -n -B build-release

# Parse only (no build)
pup parse -v

# Export build script
pup export script > build.sh

# Generate compile_commands.json
pup export compdb > compile_commands.json

# Export dependency graph (DOT format)
pup export graph | dot -Tpng -o graph.png

# Export dependency graph (summary)
pup export graph --summary
```

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
│   ├── graph/          # Dependency DAG, builder, topological sort, rule patterns
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
Pup successfully builds this project (75 Tupfiles, 681 commands).

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
cd ~/src/castlestech.com/megahunt/PPC_Linux/ctos
~/src/pup/build/pup build

# Compare with tup:
tup
```

### Current Status

- ✅ Multi-directory Tupfile scanning
- ✅ Demand-driven parsing with cycle detection
- ✅ Cross-directory order-only groups
- ✅ Variant build path resolution
- ✅ `import` directive
- ✅ `export` directive
- ✅ Bang macros

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

Pup offers two methods for tracking header dependencies:

### Method 1: Compiler-generated `.d` files (recommended)

Add `-MD` to your compile flags:
```tup
CFLAGS += -MD
: foreach *.c |> $(CC) $(CFLAGS) -c %f -o %o |> %B.o
```

**How it works:**
1. Compiler generates `foo.d` alongside `foo.o` listing all included headers
2. After successful compilation, pup parses the `.d` file
3. Discovered headers stored as `LinkType::Implicit` edges in `.pup/index`
4. On subsequent builds, changed headers trigger rebuild of dependent commands
5. **All headers tracked** including system headers (`/usr/include/*`)

### Method 2: Auto-generated dependency scanning (`PUP_IMPLICIT_DEPS`)

Set `PUP_IMPLICIT_DEPS=1` to auto-generate dependency scanning rules:
```bash
PUP_IMPLICIT_DEPS=1 pup build
```

**How it works:**
1. Pattern-matches C/C++ compile commands (gcc, clang, etc. with `-c` flag)
2. Auto-generates `gcc -M` rules to discover header dependencies
3. Generated rules execute before their parent compile commands
4. stdout parsed as depfile, headers injected as implicit edges

**Key design decisions:**
1. **Generated rules are first-class DAG nodes** - No separate "event" system; dep-scan commands are regular build rules
2. **Edge ordering** - DAG edge from dep-scan → compile ensures dep-scan runs first
3. **Stdout as abstract output** - `GeneratedOutput::Type::Stdout` extends output model beyond files
4. **InjectImplicitDeps action** - Special output action parses stdout as depfile, adds edges to parent command
5. **Pattern registry** - `RulePatternRegistry` is extensible for future patterns

**Implementation details:**
- `include/pup/graph/rule_pattern.hpp` - `RulePattern`, `GeneratedRule` types
- `src/graph/rule_pattern.cpp` - Pattern matching and GCC depfile pattern
- Pattern skips commands that already have `-M*` flags
- Supports ccache, distcc, sccache, icecc wrappers
- Preserves relevant flags: `-I`, `-D`, `-U`, `-std=`, `-isystem`, `--sysroot`

**Limitations:**
- Simple command tokenization (no shell quoting support for complex arguments)
- Pattern regex may not match all compiler invocation styles

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
- File entries (96 bytes each): id, parent_id, name, mtime, size, SHA-256 hash
- Command entries (64 bytes each): id, command, display
- Edges (24 bytes each): from, to, type (Normal, Sticky, Group, Implicit)
- String table: packed strings
- Footer (32 bytes): SHA-256 checksum

Full paths are reconstructed from the (parent_id, name) chain at load time.

## Implementation Phases

1. ✅ **Foundation** - Core types, hash, result, platform
2. ✅ **Parser** - Lexer, AST, parser, evaluator, depfile
3. ✅ **Graph** - DAG, builder, topological sort
4. ✅ **Index** - Binary format, reader/writer, implicit deps
5. ✅ **Execution** - Scheduler, command runner, incremental builds
6. 🔄 **Polish** - Edge cases, error handling, performance

## Design Decisions & Implementation Notes

### Variant Build Architecture

**Key insight**: In tup variant builds, commands run from the SOURCE directory, not the output directory. All path calculations (TUP_CWD, ROOT, TUP_VARIANT_OUTPUTDIR) are designed to work from this perspective.

**TUP_VARIANT_OUTPUTDIR** must be computed as the relative path from the SOURCE directory to the OUTPUT directory:
- For in-tree builds: `../build-variant/current_dir`
- For out-of-tree builds (`-B`): Relative path from `source_root/current_dir` to `output_root/current_dir`

**tup.config handling**: When a Tupfile references `../../tup.config`, it expects to find the config file:
- In source tree: doesn't exist (no tup.config at source root)
- In variant tree: `variant/tup.config` exists as symlink to actual config

For `-B` builds, special handling maps `tup.config` references to `output_root/tup.config`.

### Path Storage Architecture

Pup uses tup's (parent_dir, name) model for path storage:

- Each node stores basename only via `name` field (e.g., `"kernel.hex"`)
- `parent_dir` field stores directory NodeId
- Full path reconstructed via `get_full_path()` with caching
- `find_by_dir_name(parent_id, basename)` for efficient lookup
- `find_by_path()` derives from `get_full_path()` for backward compatibility

**Key APIs:**
- `graph.get_full_path(id)` - Reconstruct path from (parent_dir, name) chain
- `graph.find_by_dir_name(parent_id, name)` - O(1) lookup by parent + basename
- `graph.find_by_path(path)` - O(1) lookup by full path (computed from above)
