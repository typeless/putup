# Pup - Testing Skills

Quick reference for testing workflows and conventions.

## Running Tests

```bash
make test                                 # All tests
./build/test/unit/pup_test                # Direct execution
./build/test/unit/pup_test -s             # Verbose output
./build/test/unit/pup_test '[e2e]'        # E2E tests only
./build/test/unit/pup_test '[tag]'        # Specific tag
```

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
| `[dep_scanner]` | Dependency scanning (gcc -M) |

### E2E Test Tags

| Tag | Description |
|-----|-------------|
| `[e2e]` | All end-to-end tests |
| `[assignment]` | Variable assignment operators (`=`, `+=`, `?=`, `??=`) |
| `[build]` | Build command tests |
| `[clean]` | Clean/distclean tests |
| `[configure]` | Two-pass config generation |
| `[duplicate]` | Duplicate node detection |
| `[groups]` | Group semantics (`{group}`, `<group>`) |
| `[import]` | Import directive and environment variables |
| `[incremental]` | Incremental rebuild tests |
| `[keep-going]` | `-k` flag partial failure handling |
| `[layout]` | Project layout detection |
| `[multi-variant]` | Multi-variant parallel builds |
| `[platform]` | Platform conditionals (`ifdef`, `ifeq`) |
| `[scoped-config]` | Scoped configure commands |
| `[shell]` | Shell fixture tests (`test.sh`) |
| `[show]` | Show command (script, compdb, graph) |
| `[target]` | Target parsing tests |
| `[variant]` | Out-of-tree/variant builds, ghost nodes |

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

**Why?** The `copy_fixture()` function copies fixtures to temp directories and renames `Tupfile.fixture` → `Tupfile`. This prevents fixtures from being parsed as part of pup's own build.

### Fixture Types

1. **Simple fixtures** - Single directory with `Tupfile.fixture`
2. **Multi-directory fixtures** - Have `Tupfile.ini` at root with subdirectory `Tupfile.fixture` files
3. **Shell fixtures** - Include `test.sh` for complex test scenarios

### E2EFixture API

```cpp
auto f = E2EFixture { "fixture_name" };  // Copies to temp dir

// Pup commands
f.init()           // pup configure
f.build()          // pup build
f.clean()          // pup clean
f.parse()          // pup parse
f.configure()      // pup configure
f.pup({ args })    // pup <args>

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

For tests that need shell scripts:

```cpp
auto result = run_shell_fixture("fixture_name");  // Runs test.sh
REQUIRE(result.success());
```

The `test.sh` receives `$PUP` environment variable pointing to pup binary.

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
./build/test/unit/pup_test '[my_feature]' -s
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
KEEP_WORKDIR=1 ./build/test/unit/pup_test '[failing_test]'
```

Check `/tmp/claude/e2e_*` for the test directory.

### Verbose Pup Output

Add `-v` to pup commands in test:

```cpp
auto result = f.build({ "-v" });
```

### Print Output

```cpp
INFO("stdout: " << result.stdout_output);
INFO("stderr: " << result.stderr_output);
```
