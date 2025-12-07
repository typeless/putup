# Pup - Tup Build System Reimplementation

A modern C++23 reimplementation of the [Tup build system](https://gittup.org/tup/).

## Project Goals

1. **Compatibility** - Parse existing Tupfile/Tuprules.tup with zero modifications
2. **Modern C++23** - Minimal third-party dependencies (expected-lite, Catch2)
3. **Git-inspired index** - Custom binary format instead of SQLite
4. **Content hashing** - SHA-256 for precise change detection
5. **No FUSE** - Compute changes from index comparison
6. **No Lua** - Traditional Tupfile syntax only

## Building

```bash
tup init   # First time only
tup        # Build
./pup      # Run
```

## Testing

```bash
tup                      # Build including tests
./test/unit/pup_test     # Run all tests
./test/unit/pup_test -s  # Run with verbose output
```

## Code Style

### AAA (Almost Always Auto)
Use `auto` for all declarations:
```cpp
auto x = int{42};                    // Not: int x = 42;
auto ptr = std::make_unique<Foo>();  // Not: std::unique_ptr<Foo> ptr = ...
auto const& ref = container;         // Const references too
```

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

### Other Guidelines
- WebKit-based formatting (see `.clang-format`)
- No braces required around single statements
- Namespace contents not indented
- Avoid unnecessary comments - code should be self-documenting

## Project Structure

```
pup/
├── include/pup/
│   ├── core/           # Core types, hash, result, platform
│   ├── parser/         # Lexer, parser, AST, evaluator
│   ├── graph/          # Dependency DAG, builder, topological sort
│   ├── index/          # Binary index format, reader/writer
│   └── exec/           # Scheduler, command runner
├── src/                # Implementation files
├── test/unit/          # Catch2 unit tests
├── third_party/        # expected-lite, Catch2 amalgamated
├── Tupfile             # Build configuration
└── Tuprules.tup        # Shared build rules
```

## Reference Projects

- `/home/mural/src/tup/` - Original tup source (C)
- `/home/mural/src/castlestech.com/megahunt/PPC_Linux/spos/` - Real-world tup usage

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

## Index Format

Binary file at `.pup/index`:
- Header (40 bytes): magic, version, counts
- File entries (96 bytes each): id, path, mtime, size, SHA-256 hash
- Command entries (64 bytes each): id, command, display
- Edges (24 bytes each): from, to, type
- String table: packed strings
- SHA-256 checksum (32 bytes)

## Implementation Phases

1. ✅ **Foundation** - Core types, hash, result, platform
2. 🔄 **Parser** - Lexer, AST, parser, evaluator
3. ⏳ **Graph** - DAG, builder, topological sort
4. ⏳ **Index** - Binary format, reader/writer
5. ⏳ **Execution** - Scheduler, command runner
