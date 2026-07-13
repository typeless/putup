# Putup - Developer Guide

A build system using [Tupfile](https://gittup.org/tup/) syntax.

> **User Documentation**: See [docs/reference.md](docs/reference.md) for command reference, Tupfile syntax, and configuration.
>
> **Claude Code Plugin**: Load with `claude --plugin-dir plugins/putup` for skills on Tupfile authoring, project setup, cross-compilation, composable libraries, and contributing. External install: `/plugin marketplace add typeless/putup`.

## Design

- Uses Tup's Tupfile/Tuprules.tup format
- Git-inspired binary index (not SQLite)
- SHA-256 content hashing
- No FUSE, no Lua
- No libstdc++ (`-nostdlib++`); custom primitives: `StringId`, `Vec<T>`, `Function<Sig>`

**Design Coherence**: Ensure consistency between design documentation and implementation. No special-case handling or workarounds—if something doesn't fit the architecture, fix the design.

> **Architecture Details**: See [DESIGN.md](DESIGN.md) for internal architecture, data structures, and design rationale.

## Building & Testing

Putup builds itself (self-hosting). Requires `putup` in PATH.

```bash
make              # Configure and build (runs putup configure + putup build)
make V=1          # Build with verbose output
make test         # Run unit tests + E2E tests
make coverage     # Build instrumented, run tests, write gcovr report (needs gcovr)
make tidy         # Run clang-tidy
make iwyu         # Detect dead includes via clang-include-cleaner
make format       # Format with clang-format
make check        # Full CI: format-check + tidy + test
make clean        # Clean build artifacts
make distclean    # Full reset: remove build/
```

Or use putup directly:

```bash
putup configure -B build   # Generate build/tup.config from configs/
putup -B build             # Build
./build/putup              # Run the built binary
```

Build artifacts go to `build/`.

`CONFIG=<name>` selects `configs/<name>.config` at configure time (default `$(TUP_PLATFORM)`), e.g. `CONFIG=debug putup configure -B build`. The Windows binary is cross-compiled from Linux with clang-cl + lld-link against an xwin-splatted MSVC CRT + Windows SDK (`CONFIG=xwin`, requires the `XWIN_SPLAT` env var); its test binary runs under Wine with the `~[e2e]~[shell]` tag filter. See `configs/xwin.config` and `.github/workflows/ci.yml`.

## Testing

> **Testing Guide**: See [TESTING.md](TESTING.md) for E2E fixture conventions, test tags, and debugging tips.

```bash
make test                                 # Run all tests
./build/test/unit/putup_test                # All tests (unit + E2E)
./build/test/unit/putup_test -s             # Verbose output
./build/test/unit/putup_test '[e2e]'        # E2E tests only
./build/test/unit/putup_test '[build]'      # Build tests only
./build/test/unit/putup_test '[groups]'     # Group semantics tests ({group}, <group>)
./build/test/unit/putup_test '[clean]'      # Clean/distclean tests only
./build/test/unit/putup_test '[incremental]' # Incremental rebuild tests
./build/test/unit/putup_test '[variant]'    # Out-of-tree/variant tests
./build/test/unit/putup_test '[multi-variant]' # Multi-variant parallel builds
./build/test/unit/putup_test '[scope]'      # Scoped build tests
./build/test/unit/putup_test '[target]'     # Target parsing tests
./build/test/unit/putup_test '[shell]'      # Shell fixture tests
./build/test/unit/putup_test '[configure]'  # Two-pass config generation tests
./build/test/unit/putup_test '[strict]'     # Convention checker (--strict) tests
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
- `init()`, `build()`, `clean()`, `distclean()`, `parse()`, `pup()` - Run putup commands
- `exists()`, `is_file()`, `is_directory()`, `is_executable()` - Check paths
- `read_file()`, `write_file()`, `append_file()`, `remove_file()` - File I/O
- `run()` - Execute a program and capture output
- `run_pup_in_dir()` - Run putup from a subdirectory
- `create_symlink()`, `mkdir()` - Filesystem operations

**Environment variables:**
```cpp
auto env = EnvGuard { "VAR_NAME", "value" };  // RAII - auto-restores on scope exit
```

## Development Workflow

This project follows **Test-Driven Development (TDD)** with **BDD-style** tests.

**Tests come first. Always.**

1. **Write a failing test first** - Define expected behavior before implementation
2. **Run the test** - Verify it fails for the right reason
3. **Write minimal code** - Just enough to make the test pass
4. **Run the test** - Verify it passes
5. **Refactor** - Clean up while keeping tests green
6. **Format and lint** - Run clang-format and clang-tidy before committing

```bash
# TDD cycle
./build/test/unit/putup_test "[new_feature]"  # Run specific test (fails)
# ... implement ...
./build/test/unit/putup_test "[new_feature]"  # Run again (passes)
make test                                    # Verify no regressions
make format                                  # Format code
make tidy                                    # Run clang-tidy
make iwyu                                    # Check for dead includes
```

**For bug fixes:** Write a test that reproduces the bug first, then fix.

**For new features:** Write tests expressing the expected behavior before any implementation. Use BDD-style SCENARIO/GIVEN/WHEN/THEN for E2E tests.

Always run `make format`, `make tidy`, and `make iwyu` before finalizing changes.

## Code Style

See [STYLE.md](STYLE.md) for the complete C++ style guide.

Key points:
- Use `auto` with trailing return types
- Right-side const (`auto const&`)
- Anonymous namespaces for internal linkage
- WebKit-based formatting (see `.clang-format`)

## Project Structure

```
pup/
├── build/              # Build output (tup variant directory)
│   ├── tup.config      # Variant configuration
│   ├── putup           # Main binary
│   └── test/unit/putup_test
├── include/pup/
│   ├── cli/            # Command-line interface, options, output
│   ├── core/           # Core types, hash, result
│   ├── parser/         # Lexer, parser, AST, evaluator, depfile
│   ├── graph/          # Dependency DAG, builder, topological sort, rule patterns
│   ├── index/          # Binary index format, reader/writer
│   ├── platform/       # OS abstraction: env, file I/O, path, process (POSIX + Win32)
│   └── exec/           # Scheduler, command runner
├── src/                # Implementation files
├── test/
│   ├── unit/           # Catch2 unit + E2E tests (BDD style)
│   │   ├── test_*.cpp  # Test files
│   │   └── e2e_fixture.{hpp,cpp}  # E2E test infrastructure
│   └── e2e/fixtures/   # Test fixture data (Tupfiles, sources)
├── third_party/        # expected-lite, sha256, Catch2
├── Makefile            # Workflow wrapper (make test, make tidy, etc.)
├── Tupfile             # Build configuration
└── Tuprules.tup        # Shared build rules
```

## Reference Projects

For development context, the original tup source (C) can be found at https://github.com/gittup/tup

## Multi-Directory Cross-Compile Projects

Putup supports large multi-directory projects with cross-compilation. Key features tested with real-world projects:

- ✅ Multi-directory Tupfile scanning (75+ Tupfiles, 600+ commands)
- ✅ Demand-driven parsing with cycle detection
- ✅ Cross-directory order-only groups
- ✅ Variant build path resolution
- ✅ `import` directive (environment variables from SDK)
- ✅ `export` directive (for subprocess calls like pkg-config)
- ✅ Bang macros
- ✅ Ghost nodes for cross-directory generated file dependencies

### Example Structure

```
project/
├── Tupfile.ini          # Project root marker
├── Tuprules.tup         # Shared rules with import/export
├── build-variant/       # Variant output directory
│   └── tup.config       # Variant configuration
└── subsystems/          # Subdirectories with Tupfiles
    ├── daemon1/Tupfile
    ├── daemon2/Tupfile
    └── ...
```

### Cross-Compile Tuprules Pattern

```tup
# Import from SDK environment
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

## Implementation Phases

1. ✅ **Foundation** - Core types, hash, result, platform
2. ✅ **Parser** - Lexer, AST, parser, evaluator, depfile
3. ✅ **Graph** - DAG, builder, topological sort
4. ✅ **Index** - Binary format, reader/writer, implicit deps
5. ✅ **Execution** - Scheduler, command runner, incremental builds
6. 🔄 **Polish** - Edge cases, error handling, performance

## Design Decisions

See [DESIGN.md](DESIGN.md) for internal architecture, data structures, and design rationale.
