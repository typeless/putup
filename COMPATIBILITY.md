# Tupfile Compatibility

Putup uses Tup's Tupfile syntax. This document covers supported features and differences.

## Commands

| Tup Command | Putup Equivalent | Status |
|-------------|----------------|--------|
| `tup` | `putup` | ✅ Implemented |
| `tup upd` | `putup` | ✅ Implemented |
| `tup init` | `putup configure` | ✅ Creates tup.config and initializes project |
| `tup refactor` | `putup parse` | ✅ Implemented |
| `tup graph` | `putup show graph` | ✅ Implemented |
| `tup compiledb` | `putup show compdb` | ✅ Implemented |
| `tup monitor` | - | ❌ Not planned (no FUSE) |
| `tup stop` | - | ❌ Not planned (no monitor) |
| `tup scan` | - | ❌ Not implemented |
| `tup variant` | `putup configure -B` | ✅ Creates variant dir + tup.config |
| `tup generate` | `putup show script` | ✅ Implemented |
| `tup commandline` | - | ❌ Not implemented |
| `tup todo` | - | ❌ Not implemented |
| `tup varsed` | - | ❌ Not implemented |
| `tup options` | - | ❌ Not implemented |
| `tup dbconfig` | - | ❌ Not implemented |

## Tupfile Syntax Support

See [docs/reference.md](docs/reference.md#5-tupfile-syntax) for complete syntax documentation.

### Implementation Status

| Feature | Status |
|---------|--------|
| Rules (`: inputs \|> command \|> outputs`) | ✅ Implemented |
| foreach rules | ✅ Implemented |
| Variables (`=`, `+=`, `:=`) | ✅ Implemented |
| Soft/weak set (`?=`, `??=`) | ✅ Implemented (extension) |
| Config variables (`@(VAR)`) | ✅ Implemented |
| Node variables (`&(VAR)`) | ✅ Implemented |
| Pattern flags (`%f`, `%o`, `%B`, etc.) | ✅ Implemented |
| Bang macros (`!name`) | ✅ Implemented |
| Conditionals (`ifdef`, `ifeq`, etc.) | ✅ Implemented |
| Groups and bins (`{group}`, `<group>`) | ✅ Implemented |
| Directives (`include`, `export`, `import`) | ✅ Implemented |
| `run` directive | ❌ Not implemented |
| `preload` directive | ⚠️ Parsed, not enforced |
| `error` directive | ❌ Not implemented |

### Groups and Bins

| Feature | Syntax | Status |
|---------|--------|--------|
| Groups (bins) | `{groupname}` | ✅ |
| Order-only groups | `<groupname>` | ✅ |
| Cross-directory groups | `dir/<groupname>` | ✅ |

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

**Cross-directory groups** reference groups defined in other Tupfiles:
```tup
# In src/Tupfile - reference group from include/generated/Tupfile
: foo.c | $(ROOT)/include/generated/<gen-headers> |> $(CC) -c %f -o %o |> foo.o
```

### Multi-Directory Projects

| Feature | Status |
|---------|--------|
| Subdirectory Tupfiles | ✅ |
| Cross-directory dependencies | ✅ |
| Demand-driven parsing | ✅ |
| Per-Tupfile variable scope | ✅ |
| `include_rules` inheritance | ✅ |
| Circular dependency detection | ✅ |

Putup fully supports projects with Tupfiles in multiple subdirectories:
```
project/
├── Tuprules.tup        # Shared macros
├── src/
│   └── Tupfile         # Uses !cc from Tuprules.tup
└── tests/
    └── Tupfile         # Independent Tupfile
```

## Behavioral Differences

### Change Detection

| Aspect | Tup | Putup |
|--------|-----|-----|
| Primary method | FUSE interception | Index comparison |
| Fallback | mtime | mtime → size → SHA-256 |
| Implicit deps | Automatic via FUSE | Requires `-MD` flag |

Putup's change detection algorithm:
1. If mtime differs from index → rebuild
2. If mtime matches but size differs → rebuild
3. If size matches → compute SHA-256, rebuild if different

### Implicit Dependencies

Tup uses FUSE to intercept file accesses and automatically discover dependencies.

Putup requires explicit `.d` file generation:

```tup
CFLAGS += -MD  # Generate foo.d alongside foo.o
: foreach *.c |> $(CC) $(CFLAGS) -c %f -o %o |> %B.o
```

Putup tracks **all headers** including system headers (`/usr/include/*`).

### Database Format

| Aspect | Tup | Putup |
|--------|-----|-----|
| Format | SQLite database | Binary index file |
| Location | `.tup/db` | `.pup/index` |
| Corruption recovery | SQLite tools | Delete and rebuild |

### Directory Structure

```
# Tup                    # Putup
.tup/                    .pup/
├── db                   └── index
├── object/
└── ...
```

## Migration from Tup

### Basic Migration

1. Most Tupfiles work unchanged
2. Replace `tup` with `putup` in scripts
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

Putup includes E2E tests that verify tup-compatible behavior:

```bash
./build/test/unit/putup_test '[e2e]'
```

Test fixtures cover:
- Simple C compilation
- Multi-file projects
- Bang macros
- Groups (bins and order-only)
- Conditionals
- Incremental builds
- Variant builds
- Multi-variant parallel builds
- Multi-directory projects (tested with ctos - 75 Tupfiles, 681 commands)

## Putup-Specific Features

Features in putup that extend beyond tup:

| Feature | Description |
|---------|-------------|
| **Conditional assignments** | `?=` (soft set) and `??=` (weak set) operators |
| **Path-based variants** | `putup build-debug` instead of `putup -B build-debug` |
| **Scoped builds** | `putup build-debug/src/lib` builds only that subdirectory |
| **Single output targets** | `putup build-debug/src/lib/foo.o` rebuilds one specific output |
| **Glob patterns** | `putup 'build-*'` matches multiple variants |
| **Multi-variant parallel** | `putup -B build-debug -B build-release` or `putup build-debug build-release` |
| **Auto-variant detection** | Running `putup` from project root auto-detects all variants |
| **Variant output prefix** | Output lines are prefixed with `[variant-name]` for clarity |
| **Show formats** | `putup show graph\|script\|compdb\|var\|instructions\|index` for different output formats |
| **Content-based hashing** | SHA-256 for precise change detection beyond mtime |
| **Scoped tup.config** | Per-subdirectory configs: nearest `tup.config` in parent chain is used |
| **Configure command** | `putup configure` runs config rules and ensures `tup.config` exists |

## Known Compiler Issues

### MSVC: NRVO not applied to `make_graph()` return value

**Affected**: MSVC 19.38+ (Visual Studio 2022 17.8) with `/Zc:nrvo /std:c++20 /O2`

**Status**: Fixed. The root cause was `DirNameKeyHash`/`DirNameKeyEqual` functors holding raw `StringPool*` pointers into the same `Graph` object. When MSVC failed to apply NRVO on `make_graph()`, the move left dangling pointers. The `DirNameKey` hash map has been replaced with `std::vector<SortedPairVec> dir_children` (per-directory sorted arrays), eliminating the pointer coupling entirely. MSVC CI jobs remain disabled for other reasons (MinGW/GCC is the Windows CI target).

## Reporting Issues

If you find a Tupfile that works with tup but not putup:

1. Minimize the reproducer
2. Check if the feature is listed as "not implemented"
3. Open an issue with the Tupfile and expected behavior
