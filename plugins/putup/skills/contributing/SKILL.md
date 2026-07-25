---
name: contributing
description: Contributing to putup development. Use when building putup, running tests, writing E2E tests, understanding the architecture, or following the code style.
---

# Contributing to Putup

Putup is a build system using Tup's Tupfile syntax, written in C++20. It builds itself (self-hosting).

Full reference: <https://github.com/typeless/putup/blob/main/docs/reference.md>

## Build and Test

Putup requires `putup` in PATH for self-hosting builds.

| Command | Action |
|---------|--------|
| `make` | Configure and build (runs `putup configure` + `putup build`) |
| `make V=1` | Build with verbose output |
| `make test` | Run unit tests + E2E tests |
| `make tidy` | Run clang-tidy |
| `make iwyu` | Detect dead includes via clang-include-cleaner |
| `make format` | Format with clang-format |
| `make bootstrap` | Regenerate the committed `bootstrap-*.sh` scripts |
| `make check` | Full CI: format-check + tidy + test |
| `make coverage` | Build gcov-instrumented variant, run tests, write gcovr report to build-coverage/report/ |
| `make clean` | Clean build artifacts |
| `make distclean` | Full reset: remove build/ |

`make coverage` selects `configs/coverage.config` via the `CONFIG` env var (`CONFIG=coverage`), not `--config`. It needs `gcovr` and writes an HTML `index.html`, a Cobertura `coverage.xml`, and a JSON `summary.json` to `build-coverage/report/`.

Direct test execution:

```bash
./build/test/unit/putup_test                  # All tests
./build/test/unit/putup_test -s               # Verbose output
./build/test/unit/putup_test '[e2e]'          # E2E tests only
./build/test/unit/putup_test '[tag]'          # Specific tag
./build/test/unit/putup_test '~[e2e]~[shell]' # Exclude tags
```

Or build and run directly:

```bash
putup configure -B build
putup -B build
./build/putup
```

## Platform builds

`configs/` holds one config per platform, selected via the `CONFIG` env var (default `$(TUP_PLATFORM)`): `CONFIG=<name> putup configure -B <dir>` picks `configs/<name>.config`. Available configs include `linux`, `macosx`, `xwin`, `coverage`, `debug`, and `default`.

Windows is cross-compiled from Linux with `CONFIG=xwin`: clang-cl + lld-link + llvm-lib against an xwin-splatted MSVC CRT and Windows SDK (needs the `XWIN_SPLAT` env var, `--target=x86_64-pc-windows-msvc`, `/MT` static CRT). CI runs the resulting test binary natively on a `windows-latest` runner, excluding `[e2e]` and `[shell]` tags (Wine runs the same binary locally):

```bash
./build/test/unit/putup_test '~[e2e]~[shell]'
```

## TDD Workflow

Tests come first. Always.

1. **Write a failing test** -- define expected behavior before implementation
2. **Run the test** -- verify it fails for the right reason
3. **Write minimal code** -- just enough to make it pass
4. **Run the test** -- verify it passes
5. **Refactor** -- clean up while tests stay green
6. **Format and lint** -- `make format && make tidy`

```bash
./build/test/unit/putup_test '[new_feature]'  # RED: fails
# ... implement ...
./build/test/unit/putup_test '[new_feature]'  # GREEN: passes
make test                                     # No regressions
make format && make tidy                      # Clean up
```

For bug fixes: write a test that reproduces the bug first, then fix.

## Test Architecture

All tests live in `test/unit/` and use Catch2 with BDD-style macros (`SCENARIO/GIVEN/WHEN/THEN`).

### Test tags

**Unit:** `[lexer]` `[parser]` `[eval]` `[builder]` `[graph]` `[index]` `[exec]` `[glob]` `[hash]` `[path_utils]` `[dep_scanner]`

**E2E:** `[e2e]` `[build]` `[clean]` `[configure]` `[groups]` `[import]` `[incremental]` `[variant]` `[multi-variant]` `[scope]` `[strict]` `[shell]`

## Writing E2E Tests

### E2EFixture API

```cpp
auto f = E2EFixture { "fixture_name" };  // Copies test/e2e/fixtures/fixture_name/ to temp dir

// Putup commands (return result with .success(), .stdout_output, .stderr_output)
f.init();  f.build();  f.build({ "-v" });  f.clean();  f.distclean();  f.parse();
f.pup({ "show", "compdb" });

// Filesystem checks
f.exists("path");  f.is_file("path");  f.is_directory("path");  f.is_executable("path");

// File I/O
f.read_file("path");  f.write_file("path", "content");
f.append_file("path", "content");  f.remove_file("path");

// Other
f.run("./program");  f.run_pup_in_dir("subdir");  f.mkdir("path");
f.create_symlink("target", "link");

// Environment variables (RAII -- restores original on scope exit)
auto env = EnvGuard { "VAR_NAME", "value" };
```

### Writing a test

```cpp
SCENARIO("Feature description", "[e2e][feature_tag]")
{
    GIVEN("an initialized project")
    {
        auto f = E2EFixture { "fixture_name" };
        REQUIRE(f.init().success());

        WHEN("the project is built")
        {
            auto result = f.build();

            THEN("the output exists")
            {
                REQUIRE(result.success());
                REQUIRE(f.is_executable("output"));
            }
        }
    }
}
```

## Fixture Conventions

### File naming

Fixtures use `Tupfile.fixture` naming. The `E2EFixture` constructor copies fixtures to a temp dir and renames `Tupfile.fixture` to `Tupfile`. This prevents fixtures from being parsed during putup's own build.

```
test/e2e/fixtures/
  simple_c/
    hello.c
    Tupfile.fixture          # Renamed to Tupfile during test
  multi_dir/
    Tupfile.ini              # Project root marker
    lib/
      Tupfile.fixture
    app/
      Tupfile.fixture
```

### Fixture types

| Type | Structure | Use case |
|------|-----------|----------|
| Simple | Single dir with `Tupfile.fixture` | Single-rule tests |
| Multi-directory | `Tupfile.ini` + subdirs | Cross-directory deps, groups |
| Shell | Includes `test.sh` | Complex multi-step scenarios |

### Shell fixtures

```cpp
auto result = run_shell_fixture("fixture_name");  // Runs test.sh in fixture dir
REQUIRE(result.success());
```

The `test.sh` receives `$PUP` pointing to the putup binary:

```bash
#!/bin/bash
set -e
$PUP configure
$PUP
test -f expected_output.txt
```

## Debugging Tests

Keep the work directory for inspection:

```bash
KEEP_WORKDIR=1 ./build/test/unit/putup_test '[failing_test]'
# Inspect /tmp/claude/e2e_* for the test directory
```

Print output inside tests:

```cpp
INFO("stdout: " << result.stdout_output);
INFO("stderr: " << result.stderr_output);
```

Pass `-v` for verbose putup output:

```cpp
auto result = f.build({ "-v" });
```

## Differential Testing Against Real Tup

For semantic questions ("what should this Tupfile do?"), real tup is the
reference. Build the same source tree with both and diff the output trees:

```bash
# Same source dir, two variants — identical commands, comparable bytes
tup variant configs/board.config && tup
putup configure --config configs/board.config -B build-putup && putup -B build-putup

cd build-board && find . -type f | grep -v '^./\.tup' | sort > /tmp/a
cd ../build-putup && find . -type f | grep -v '^./\.pup' | sort > /tmp/b
diff /tmp/a /tmp/b                  # same file set?
# then cmp matching pairs for bit-identity
```

- Run both from the **same source directory**: `-g` embeds the absolute
  compile dir (`DW_AT_comp_dir`), so separate checkouts differ spuriously.
- Variant directory names leak into outputs via `%f` paths (e.g.
  `objdump -S %f` prints the input path) — normalize before diffing.
- A stale-but-plausible output (wrong bytes, never rebuilt) means putup ran a
  different command than tup: compare `putup show index PATTERN` against the
  rule to find the divergence.
- Neutralize the environment: fix `SOURCE_DATE_EPOCH`, and defeat compiler
  caches (projects that wrap commands via `import CCACHE_EXEC=` run raw with
  `CCACHE_EXEC=` set empty).

This found the conditional bang-macro scoping bug: 1830-file trees diffed in
exactly 3 files, and the index showed putup had executed the other `ifeq`
branch's `!hex_cat`.

## Project Structure

```
pup/
  include/pup/
    cli/          # Command-line interface, options, output
    core/         # Core types, hash, result, platform
    parser/       # Lexer, parser, AST, evaluator, depfile
    graph/        # Dependency DAG, builder, topological sort, rule patterns
    index/        # Binary index format, reader/writer
    exec/         # Scheduler, command runner
  src/            # Implementation files (mirrors include/ layout)
  configs/        # Per-platform configs, selected via CONFIG env var
  test/
    unit/         # Catch2 tests (test_*.cpp) + e2e_fixture.{hpp,cpp}
    e2e/fixtures/ # Test fixture data
  third_party/    # expected-lite, sha256, Catch2
  Makefile        # Workflow wrapper
  Tupfile         # Build configuration
  Tuprules.tup    # Shared build rules
```

## Design Principles

- **No libstdc++** -- binary links with `-nostdlib++`, zero runtime dependency
- **Custom primitives** -- `StringId` (4-byte interned string), `Vec<T>` (growable array), `Function<Sig>` (type-erased callable), `SteadyClock` (monotonic timer)
- **Git-inspired index** -- binary format, not SQLite; SHA-256 content hashing
- **No FUSE** -- uses explicit `-MD` flags for header tracking
- **No Lua** -- pure Tupfile syntax, no scripting extensions
- **Result<T>** -- all fallible operations return `Result<T>`, never throw exceptions
- **Design coherence** -- no workarounds. If something does not fit the architecture, fix the design.
