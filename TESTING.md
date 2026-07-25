# Putup - Testing Skills

Quick reference for testing workflows and conventions.

## Running Tests

```bash
make test                                 # Build anything stale, then run tests (cached)
./build/putup -B build -j16 test/runner/    # Same thing, directly
./build/putup --rerun -B build -j16 test/runner/  # Force rerun without changes (flake check)
./build/test/unit/putup_test                # Direct execution (serial)
./build/test/unit/putup_test -s             # Verbose output
./build/test/unit/putup_test '[e2e]'        # E2E tests only
./build/test/unit/putup_test '[tag]'        # Specific tag
```

Tests run as build rules in the `test/runner/` nested project (pruned from
plain builds; composed by targeting it): one rule for the fast `~[e2e]` suite,
32 Catch2 `--shard-count` shards for `[e2e]`, and a summary join. Results are
cached — unchanged inputs (test binary, putup binary, `test/e2e/fixtures/**`)
mean "Nothing to do"; `--rerun` overrides. Shard logs live in
`build/test/runner/` and a failed shard's log prints with the failure.
The E2E shards are subprocess-bound, so `-j` above core count pays off.
CI uses `test/run-tests.sh` (a shard-parallel script) instead: CI test jobs
run from bootstrap-built artifacts, which have no `.pup` index for the graph
runner to build on.

### Unit Test Tags

| Tag | Description |
|-----|-------------|
| `[lexer]` | Tokenizer tests |
| `[parser]` | AST parsing tests |
| `[eval]` | Variable evaluation |
| `[builder]` | Graph building |
| `[graph]` | DAG operations |
| `[index]` | Binary index format |
| `[exec]` | Command execution |
| `[glob]` | Glob pattern matching |
| `[hash]` | SHA-256 hashing |
| `[path_utils]` | Path manipulation |
| `[dep_scanner]` | Dependency scanning (gcc `-M`, clang-cl `/clang:-M`) |
| `[clang_cl]` | clang-cl (MSVC driver) dependency scanning |

### E2E Test Tags

| Tag | Description |
|-----|-------------|
| `[e2e]` | All end-to-end tests |
| `[assignment]` | Variable assignment operators (`=`, `+=`, `?=`, `??=`) |
| `[build]` | Build command tests |
| `[clean]` | Clean/distclean tests |
| `[configure]` | Two-pass config generation |
| `[duplicate]` | Duplicate node detection |
| `[exclude]` | `-x` directory exclusion |
| `[groups]` | Group semantics (`{group}`, `<group>`) |
| `[import]` | Import directive and environment variables |
| `[incremental]` | Incremental rebuild tests |
| `[keep-going]` | `-k` flag partial failure handling |
| `[layout]` | Project layout detection |
| `[multi-variant]` | Multi-variant parallel builds |
| `[platform]` | Platform conditionals (`ifdef`, `ifeq`) |
| `[scope]` | Scoped build tests (mm/mma behavior, `-A` flag) |
| `[scoped-config]` | Scoped configure commands |
| `[shell]` | Shell fixture tests (`test.sh`) |
| `[show]` | Show command (script, compdb, graph) |
| `[strict]` | Convention checker (`--check` / `--strict`) |
| `[target]` | Target parsing tests |
| `[variant]` | Out-of-tree/variant builds, ghost nodes |

## Windows Tests

The Windows test binary is cross-compiled from Linux with `CONFIG=xwin`
(clang-cl + xwin against the MSVC CRT / Windows SDK):

```bash
XWIN_SPLAT="$HOME/.xwin/splat" CONFIG=xwin putup configure -B build-win
XWIN_SPLAT="$HOME/.xwin/splat" CONFIG=xwin putup -B build-win
```

CI splits that across two jobs in `.github/workflows/ci.yml`: `build-windows`
cross-compiles on Linux and uploads `putup_test.exe`, then `test-windows`
downloads it and runs it **natively on a `windows-latest` runner**. E2E and
shell fixtures are excluded because they rely on a POSIX shell:

```bash
putup_test.exe "~[e2e]~[shell]"
```

Locally, Wine runs the same binary without a Windows host:

```bash
wine build-win/test/unit/putup_test.exe "~[e2e]~[shell]"
```

Treat a Wine pass as corroboration, not proof — Wine reimplements the Win32 API
and the CRT, so behaviour can diverge. Moving CI off Wine (commit `27d142ea`)
immediately exposed an `abort()` fault the Wine job had masked.

## Code Coverage

```bash
make coverage                 # build instrumented, run tests, write report
```

`make coverage` builds a separate gcov-instrumented variant in `build-coverage/`
(from `configs/coverage.config`), runs the full suite with `PUP` pointed at the
instrumented binary so E2E subprocess runs count too, then aggregates with
[gcovr](https://gcovr.com):

- Terminal summary (line / function / branch %)
- `build-coverage/report/index.html` — browsable HTML, drill down per file
- `build-coverage/report/coverage.xml` — Cobertura XML
- `build-coverage/report/summary.json` — machine-readable totals

Coverage is measured over `src/` and `include/pup/` only (third-party and test
code are excluded). CI runs the same target and uploads the HTML report as the
`coverage-report` artifact.

**Requirements:** `gcovr` (`pipx install gcovr` or `pip install --user gcovr`)
and a `gcov` matching your `g++`. The Makefile auto-selects `gcov-<major>` when
present; override with `make coverage GCOV=gcov-14` if detection is wrong.

The coverage variant uses `-O0 --coverage` and keeps `-nostdlib++`, so it is for
measurement only — the shipped binary still comes from the optimized build.

## E2E Fixture Conventions

### File Naming

All fixtures use `Tupfile.fixture` naming:

```
test/e2e/fixtures/
├── simple_c/
│   ├── hello.c
│   └── Tupfile.fixture    # ← Renamed to Tupfile during test
├── multi_dir/
│   ├── Tupfile.ini        # Project root marker
│   ├── lib/
│   │   └── Tupfile.fixture
│   └── app/
│       └── Tupfile.fixture
```

**Why?** The `copy_fixture()` function copies fixtures to temp directories and renames `Tupfile.fixture` → `Tupfile`. This prevents fixtures from being parsed as part of putup's own build.

### Fixture Types

1. **Simple fixtures** - Single directory with `Tupfile.fixture`
2. **Multi-directory fixtures** - Have `Tupfile.ini` at root with subdirectory `Tupfile.fixture` files
3. **Shell fixtures** - Include `test.sh` for complex test scenarios

### E2EFixture API

```cpp
auto f = E2EFixture { "fixture_name" };  // Copies to temp dir

// Putup commands
f.init()           // putup configure (initializes .pup directory)
f.build()          // putup build
f.clean()          // putup clean
f.parse()          // putup parse
f.pup({ args })    // putup <args> (generic command)

// Filesystem checks
f.exists("path")
f.is_file("path")
f.is_directory("path")
f.is_executable("path")

// File I/O
f.read_file("path")
f.write_file("path", "content")
f.append_file("path", "content")
f.remove_file("path")

// Other
f.run("command")           // Execute and capture output
f.run_pup_in_dir("subdir") // Run pup from subdirectory
f.mkdir("path")
f.create_symlink("target", "link")
```

### Environment Variables

```cpp
auto env = EnvGuard { "VAR_NAME", "value" };  // RAII - restores on scope exit
```

### Shell Fixtures

For tests that need shell scripts, use `run_shell_fixture()` (defined in `e2e_fixture.{hpp,cpp}`):

```cpp
auto result = run_shell_fixture("fixture_name");  // Runs test.sh in fixture dir
REQUIRE(result.success());
```

The `test.sh` script receives the `$PUP` environment variable pointing to the putup binary. The fixture directory must contain a `test.sh` file that performs the test and exits with 0 on success.

Example `test.sh`:
```bash
#!/bin/bash
set -e
$PUP configure
$PUP
test -f expected_output.txt
```

## Adding New Tests

### 1. Create Fixture

```bash
mkdir -p test/e2e/fixtures/my_feature/
```

Add source files and `Tupfile.fixture`:

```bash
echo ': input.txt |> cat %f > %o |> output.txt' > test/e2e/fixtures/my_feature/Tupfile.fixture
echo 'hello' > test/e2e/fixtures/my_feature/input.txt
```

### 2. Write Test

In `test/unit/test_e2e.cpp`:

```cpp
SCENARIO("My feature works", "[e2e][my_feature]")
{
    GIVEN("a project with my feature")
    {
        auto f = E2EFixture { "my_feature" };
        REQUIRE(f.init().success());

        WHEN("project is built")
        {
            auto result = f.build();

            THEN("output is generated correctly")
            {
                REQUIRE(result.success());
                REQUIRE(f.exists("output.txt"));
                REQUIRE(f.read_file("output.txt") == "hello\n");
            }
        }
    }
}
```

### 3. Run Test

```bash
./build/test/unit/putup_test '[my_feature]' -s
```

## Recent Features

### Assignment Operators

| Operator | Name | Behavior |
|----------|------|----------|
| `=` | Set | Always sets the variable |
| `+=` | Append | Appends to existing value |
| `?=` | Soft set | Sets only if undefined (first wins) |
| `??=` | Weak set | Deferred default (last wins, applied before rules) |

Example fixture for testing `?=`:
```
# test/e2e/fixtures/soft_assign/Tupfile.fixture
VAR ?= default
VAR ?= ignored
: |> echo $(VAR) |>
```

### Ghost Nodes (Variant Builds)

When testing cross-directory dependencies in variant builds, ghost nodes handle references to files that don't exist yet during parse.

Related fixtures:
- `variant_cross_dir_order_only/` - Order-only deps across directories
- `variant_cross_dir_regular_input/` - Regular input deps (alphabetical parse order)

## Debugging Tests

### Keep Work Directory

```bash
KEEP_WORKDIR=1 ./build/test/unit/putup_test '[failing_test]'
```

Check `/tmp/claude/e2e_*` for the test directory.

### Verbose Putup Output

Add `-v` to putup commands in test:

```cpp
auto result = f.build({ "-v" });
```

### Print Output

```cpp
INFO("stdout: " << result.stdout_output);
INFO("stderr: " << result.stderr_output);
```
