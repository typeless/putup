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

`CONFIG=<name>` selects `configs/<name>.config` at configure time (default `$(TUP_PLATFORM)`), e.g. `CONFIG=debug putup configure -B build`. The Windows binary is cross-compiled from Linux with clang-cl + lld-link against an xwin-splatted MSVC CRT + Windows SDK (`CONFIG=xwin`, requires the `XWIN_SPLAT` env var); CI runs its test binary natively on a `windows-latest` runner with the `~[e2e]~[shell]` tag filter (Wine runs the same binary locally, but is not what CI gates on). See `configs/xwin.config` and `.github/workflows/ci.yml`.

## Testing

> **Testing Guide**: See [TESTING.md](TESTING.md) for E2E fixture conventions, test tags, and debugging tips.

```bash
make test                                 # Run all tests
./build/test/unit/putup_test                # All tests (unit + E2E)
./build/test/unit/putup_test -s             # Verbose output
./build/test/unit/putup_test '[e2e]'        # Filter by tag (full tag list in TESTING.md)
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

## Reference Projects

For development context, the original tup source (C) can be found at https://github.com/gittup/tup

## Design Decisions

See [DESIGN.md](DESIGN.md) for internal architecture, data structures, and design rationale.
