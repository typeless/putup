# Pup - Developer Guide

A build system using [Tupfile](https://gittup.org/tup/) syntax.

> **User Documentation**: See [docs/reference.md](docs/reference.md) for command reference, Tupfile syntax, and configuration.

## Design

- Uses Tup's Tupfile/Tuprules.tup format
- Git-inspired binary index (not SQLite)
- SHA-256 content hashing
- No FUSE, no Lua

> **Architecture Details**: See [DESIGN.md](DESIGN.md) for internal architecture, data structures, and design rationale.

## Building & Testing

Pup builds itself (self-hosting). Requires `pup` in PATH.

```bash
make              # Configure and build (runs pup configure + pup build)
make V=1          # Build with verbose output
make test         # Run unit tests + E2E tests
make tidy         # Run clang-tidy
make format       # Format with clang-format
make check        # Full CI: format-check + tidy + test
make clean        # Clean build artifacts
make distclean    # Full reset: remove build/
```

Or use pup directly:

```bash
pup configure -B build   # Generate build/tup.config from configs/
pup -B build             # Build
./build/pup              # Run the built binary
```

Build artifacts go to `build/`.

## Testing

> **Testing Skills**: See [SKILL.md](SKILL.md) for E2E fixture conventions, test tags, and debugging tips.

```bash
make test                                 # Run all tests
./build/test/unit/pup_test                # All tests (unit + E2E)
./build/test/unit/pup_test -s             # Verbose output
./build/test/unit/pup_test '[e2e]'        # E2E tests only
./build/test/unit/pup_test '[build]'      # Build tests only
./build/test/unit/pup_test '[groups]'     # Group semantics tests ({group}, <group>)
./build/test/unit/pup_test '[clean]'      # Clean/distclean tests only
./build/test/unit/pup_test '[incremental]' # Incremental rebuild tests
./build/test/unit/pup_test '[variant]'    # Out-of-tree/variant tests
./build/test/unit/pup_test '[multi-variant]' # Multi-variant parallel builds
./build/test/unit/pup_test '[scope]'      # Scoped build tests
./build/test/unit/pup_test '[target]'     # Target parsing tests
./build/test/unit/pup_test '[shell]'      # Shell fixture tests
./build/test/unit/pup_test '[configure]'  # Two-pass config generation tests
```

### Writing E2E Tests

E2E tests use BDD style with `SCENARIO/GIVEN/WHEN/THEN` macros. Use the `E2EFixture` class to manage test fixtures:

```cpp
SCENARIO("Feature description", "[e2e][tag]")
{
    GIVEN("an initialized project")
    {
        auto f = E2EFixture { "fixture_name" };  // From test/e2e/fixtures/
        REQUIRE(f.init().success());

        WHEN("something happens")
        {
            auto result = f.build();

            THEN("expected outcome")
            {
                REQUIRE(result.success());
                REQUIRE(f.is_executable("output"));
            }
        }
    }
}
```

**E2EFixture methods:**
- `init()`, `build()`, `clean()`, `distclean()`, `parse()`, `configure()` - Run pup commands
- `exists()`, `is_file()`, `is_directory()`, `is_executable()` - Check paths
- `read_file()`, `write_file()`, `append_file()`, `remove_file()` - File I/O
- `run()` - Execute a program and capture output
- `run_pup_in_dir()` - Run pup from a subdirectory
- `create_symlink()`, `mkdir()` - Filesystem operations

**Environment variables:**
```cpp
auto env = EnvGuard { "VAR_NAME", "value" };  // RAII - auto-restores on scope exit
```

## Development Workflow

This project follows **Test-Driven Development (TDD)** with **BDD-style** tests:

1. **Write a failing test first** - Define expected behavior before implementation
2. **Make it pass** - Write minimal code to satisfy the test
3. **Refactor** - Clean up while keeping tests green
4. **Format and lint** - Run clang-format and clang-tidy before committing

```bash
# TDD cycle
./build/test/unit/pup_test "[new_feature]"  # Run specific test (fails)
# ... implement ...
./build/test/unit/pup_test "[new_feature]"  # Run again (passes)
make test                                    # Verify no regressions
make format                                  # Format code
make tidy                                    # Run clang-tidy
```

For bug fixes, write a test that reproduces the bug first, then fix.

For new features, use BDD-style SCENARIO/GIVEN/WHEN/THEN to express behavior.

Always run `make format` and `make tidy` before finalizing changes.

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
│   ├── unit/           # Catch2 unit + E2E tests (BDD style)
│   │   ├── test_*.cpp  # Test files
│   │   └── e2e_fixture.{hpp,cpp}  # E2E test infrastructure
│   └── e2e/fixtures/   # Test fixture data (Tupfiles, sources)
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
~/src/pup/build/pup

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
- ✅ Ghost nodes for cross-directory generated file dependencies

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

### Command Execution Rules

**Rule 1: Working Directory**
Commands always execute from the Tupfile's source directory, not from the output directory or project root.

**Rule 2: Path Expansion**
All paths in commands (`%f`, `%o`, etc.) are relative to the Tupfile's directory:
- Local files: `add.c` stays as `add.c` (not `../../src/lib/add.c`)
- Cross-directory references: `../../include/foo.h` preserved as-is
- Variant outputs: `../../build/src/lib/add.o` (relative to source dir)

**Implementation detail**: Pup stores paths project-root-relative internally for graph consistency, then transforms them to Tupfile-relative during command expansion:
- If path starts with `current_dir/`, strip the prefix (local file)
- If path starts with `..`, keep as-is (already relative)
- Otherwise, prepend `../` for each directory level (cross-directory)

### Path Storage Architecture

**Deviation from tup**: tup stores its database at `<project_root>/.tup/` and all paths are source-root-relative. Pup supports true out-of-tree builds (`-B`), so the index lives in the build root. This requires paths to be stored relative to the build root (`-B` directory), not the source root.

Pup uses a layered path architecture with clear separation between storage and presentation:

```
┌─────────────────────────────────────────────────────────────────┐
│                     Command Execution Layer                     │
│  Paths relative to Tupfile directory (what compiler sees)       │
│  e.g., "add.c", "../../build/src/lib/add.o"                     │
└─────────────────────────────────────────────────────────────────┘
                              ↑
                    Translation at expand_command()
                              ↑
┌─────────────────────────────────────────────────────────────────┐
│                     Index Storage Layer                         │
│  Paths relative to index location (-B dir or source root)       │
│  e.g., "src/lib/add.c", "src/lib/add.o"                         │
└─────────────────────────────────────────────────────────────────┘
```

**Index Storage (`.pup/index` in -B dir)**:
- Paths stored relative to the build root (-B directory)
- Uses tup's (parent_dir, name) model: basename + parent NodeId
- Makes index self-contained and portable
- Full paths reconstructed via `get_full_path()` with caching

**Command Execution**:
- Commands run from Tupfile's source directory (not -B dir)
- Paths translated to be relative to working directory
- Local files: `src/lib/add.c` → `add.c` (strip current_dir prefix)
- Cross-directory: `include/foo.h` → `../../include/foo.h` (prepend `../`)
- Variant outputs: `src/lib/add.o` → `../../build/src/lib/add.o`

**Key insight**: The index stores "what exists where" (build-root-relative), while commands express "how to build" (Tupfile-relative). These are different coordinate systems requiring translation.

**Key APIs:**
- `graph.get_full_path(id)` - Reconstruct path from (parent_dir, name) chain
- `graph.find_by_dir_name(parent_id, name)` - O(1) lookup by parent + basename
- `expand_command()` - Translates index paths to Tupfile-relative paths

### Ghost Node Semantics

Ghost nodes handle cross-directory dependencies in variant builds where parse order matters.

**Problem**: Directories are parsed alphabetically. If `aaa_consumer/Tupfile` references `../zzz_producer/helper.c` (a generated file), the producer hasn't been parsed yet.

**Solution**:
1. `resolve_input_node()` creates a Ghost node for non-existent files
2. Dependency edges are established from command → ghost
3. When `zzz_producer` is parsed, Ghost upgrades to Generated
4. **Critical**: Edges are preserved during upgrade (not deleted)

**Key implementation files**:
- `src/graph/builder.cpp`: `resolve_input_node()` creates ghosts, `get_or_create_file_node()` handles upgrade
- `src/exec/scheduler.cpp`: Validates no unresolved ghosts before build
- `src/cli/cmd_build.cpp`: Skips ghosts during index serialization

**Difference from Tup**: Tup deletes edges during Ghost→Generated upgrade and re-parses dependent Tupfiles. Pup preserves edges because it parses all Tupfiles fresh each build—the edges are already correct.

**Testing**: See `test/e2e/fixtures/variant_cross_dir_*` fixtures and `[variant]` test tag.
