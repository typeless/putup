# Tup Compatibility Guide

This document details pup's compatibility with tup, including supported features, known differences, and migration notes.

## Commands

| Tup Command | Pup Equivalent | Status |
|-------------|----------------|--------|
| `tup` | `pup build` | ✅ Implemented |
| `tup upd` | `pup build` | ✅ Implemented |
| `tup init` | `pup init` | ✅ Implemented |
| `tup refactor` | `pup parse` | ✅ Implemented |
| `tup graph` | `pup graph` | ✅ Implemented |
| `tup monitor` | - | ❌ Not planned (no FUSE) |
| `tup stop` | - | ❌ Not planned (no monitor) |
| `tup scan` | - | ❌ Not implemented |
| `tup variant` | - | ❌ Not implemented |
| `tup generate` | - | ❌ Not implemented |
| `tup compiledb` | - | ❌ Not implemented |
| `tup commandline` | - | ❌ Not implemented |
| `tup todo` | - | ❌ Not implemented |
| `tup varsed` | - | ❌ Not implemented |
| `tup options` | - | ❌ Not implemented |
| `tup dbconfig` | - | ❌ Not implemented |

## Tupfile Syntax

### Rules

```tup
: [foreach] inputs [| order-only] |> command |> outputs [{group}]
```

✅ **Fully supported** including:
- `foreach` for per-input rule expansion
- Multiple inputs and outputs
- Order-only dependencies with `|`
- Output groups with `{groupname}`
- Display text with `^ text ^`

### Variables

| Syntax | Description | Status |
|--------|-------------|--------|
| `VAR = value` | Assignment | ✅ |
| `VAR += value` | Append | ✅ |
| `VAR := value` | Immediate (no expansion) | ✅ |
| `$(VAR)` | Variable reference | ✅ |
| `@(VAR)` | Config variable from tup.config | ✅ |
| `&(VAR)` | Node variable | ✅ |

### Special Variables

| Variable | Description | Status |
|----------|-------------|--------|
| `$(TUP_CWD)` | Current Tupfile directory | ✅ |
| `$(TUP_PLATFORM)` | Operating system | ✅ |
| `$(TUP_ARCH)` | CPU architecture | ✅ |
| `$(TUP_VARIANTDIR)` | Relative path to variant | ✅ |
| `$(TUP_VARIANT_OUTPUTDIR)` | Absolute variant path | ✅ |

### Pattern Flags

| Flag | Description | Status |
|------|-------------|--------|
| `%f` | All inputs | ✅ |
| `%o` | All outputs | ✅ |
| `%b` | Basename with extension | ✅ |
| `%B` | Basename without extension | ✅ |
| `%e` | Extension (foreach only) | ✅ |
| `%d` | Directory name | ✅ |
| `%g` | Glob match portion | ✅ |
| `%O` | Output directory | ✅ |
| `%1f`, `%2f` | Nth input | ✅ |
| `%1o`, `%2o` | Nth output | ✅ |
| `%%` | Literal % | ✅ |

### Bang Macros

```tup
!cc = |> ^ CC %o^ $(CC) $(CFLAGS) -c %f -o %o |> %B.o
: foreach *.c |> !cc |> {objs}
```

✅ **Fully supported** including:
- Macro definition with command and outputs
- Display text in macros
- Macro invocation in rules

### Conditionals

| Syntax | Status |
|--------|--------|
| `ifdef VAR` | ✅ |
| `ifndef VAR` | ✅ |
| `ifeq ($(VAR),value)` | ✅ |
| `ifneq ($(VAR),value)` | ✅ |
| `else` | ✅ |
| `endif` | ✅ |

### Directives

| Directive | Description | Status |
|-----------|-------------|--------|
| `include path` | Include file | ✅ |
| `include_rules` | Include Tuprules.tup chain | ✅ |
| `.gitignore` | Generate .gitignore for outputs | ✅ |
| `export VAR` | Export to command environment | ✅ |
| `import VAR[=default]` | Import from environment | ✅ |
| `run ./script` | Execute script for rules | 🔶 Parsed only |
| `preload dir` | Allow wildcards in subdir | 🔶 Parsed only |
| `error message` | Halt with error | 🔶 Parsed only |

### Groups and Bins

| Feature | Syntax | Status |
|---------|--------|--------|
| Groups (bins) | `{groupname}` | ✅ |
| Order-only groups | `<groupname>` | ✅ |

**Groups** (tup calls these "bins") collect outputs for use as inputs in other rules:
```tup
: foreach *.c |> $(CC) -c %f -o %o |> %B.o {objs}
: {objs} |> $(CC) -o %o %f |> program
```

**Order-only groups** are for cross-directory order-only dependencies:
```tup
: gen-headers.sh |> ./gen-headers.sh |> headers.h <gen>
: foo.c | <gen> |> $(CC) -c %f -o %o |> foo.o
```

## Behavioral Differences

### Change Detection

| Aspect | Tup | Pup |
|--------|-----|-----|
| Primary method | FUSE interception | Index comparison |
| Fallback | mtime | mtime → size → SHA-256 |
| Implicit deps | Automatic via FUSE | Requires `-MD` flag |

Pup's change detection algorithm:
1. If mtime differs from index → rebuild
2. If mtime matches but size differs → rebuild
3. If size matches → compute SHA-256, rebuild if different

### Implicit Dependencies

Tup uses FUSE to intercept file accesses and automatically discover dependencies.

Pup requires explicit `.d` file generation:

```tup
CFLAGS += -MD  # Generate foo.d alongside foo.o
: foreach *.c |> $(CC) $(CFLAGS) -c %f -o %o |> %B.o
```

Pup tracks **all headers** including system headers (`/usr/include/*`).

### Database Format

| Aspect | Tup | Pup |
|--------|-----|-----|
| Format | SQLite database | Binary index file |
| Location | `.tup/db` | `.pup/index` |
| Corruption recovery | SQLite tools | Delete and rebuild |

### Directory Structure

```
# Tup                    # Pup
.tup/                    .pup/
├── db                   └── index
├── object/
└── ...
```

## Migration from Tup

### Basic Migration

1. Most Tupfiles work unchanged
2. Replace `tup` with `pup` in scripts
3. Add `-MD` to compiler flags for header tracking

### Feature Workarounds

#### `run` directive

Tup:
```tup
run ./generate-sources.sh
```

Workaround: Run the script manually before building, or use a rule:
```tup
: generate-sources.sh |> ./generate-sources.sh |> generated.c
```

## Testing Compatibility

Pup includes E2E tests that verify tup-compatible behavior:

```bash
./test/e2e/run_tests.sh
```

Test fixtures cover:
- Simple C compilation
- Multi-file projects
- Bang macros
- Groups
- Conditionals
- Incremental builds
- Variant builds

## Reporting Issues

If you find a Tupfile that works with tup but not pup:

1. Minimize the reproducer
2. Check if the feature is listed as "not implemented"
3. Open an issue with the Tupfile and expected behavior
