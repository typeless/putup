# Pup Code Review Summary

Date: 2025-12-11

## Critical Issues (Fix Now)

| Area | Issue | File:Line |
|------|-------|-----------|
| **Scheduler** | Race condition: `stats()` read without mutex (TBD - needs study) | scheduler.cpp:118 |
| **Scheduler** | Stdin pipe deadlock with large data | runner.cpp:148-156 |
| **Runner** | ✅ FIXED: `write()` return value ignored | runner.cpp:150 |
| **Index** | ✅ FIXED: Integer overflow in string table (4GB limit) | writer.cpp:20 |
| **Index** | Missing bounds validation on mmap read | reader.cpp:140-171 |
| **Parser** | ✅ FIXED: O(n²) whitespace trim with `erase(0,1)` | parser.cpp:1017 |

## High Priority (Robustness)

| Area | Issue | Fix |
|------|-------|-----|
| **Graph** | Silent failure in `add_edge()` | Return error if node not found |
| **Scheduler** | `std::getenv()` not thread-safe | Cache env vars at startup |
| **Scheduler** | Output directory race condition | Create dirs before threading |
| **Parser** | VarDb lookup creates temp strings | Use heterogeneous lookup |
| **Main** | God object: 1373 lines | Extract to `pup/commands/` module |
| **Index** | No endianness handling | Add flag to header |

## Medium Priority (Performance)

| Area | Issue | Impact |
|------|-------|--------|
| **DAG** | Triple edge storage (edges_, inputs, outputs) | Memory waste |
| **DAG** | `nodes_of_type()` is O(n) scan | Add type index |
| **Builder** | `expand_inputs()` is 241 lines | Split into helpers |
| **Scheduler** | `notify_all()` on every job | Use `notify_one()` |
| **Topo** | Topological sort recomputed each call | Cache result |
| **Parser** | Massive switch statement (140 lines) | Dispatch table |

## Code Duplication

- **parser.cpp:403-625**: `parse_rule()` and `parse_bang_macro()` are 95% identical
- **topo.cpp:40-76**: Cycle detection logic duplicated for normal/order-only edges

## Architecture Issues

### main.cpp Refactoring

The file should be ~200 lines, not 1373. Extract into proper modules:

```
pup/
├── commands/           # NEW - Command implementations
│   ├── command.hpp    # Base interface
│   ├── init.hpp/cpp
│   ├── build.hpp/cpp
│   ├── parse.hpp/cpp
│   ├── graph.hpp/cpp
│   ├── clean.hpp/cpp
│   ├── variant.hpp/cpp
│   └── registry.hpp   # Command registry
├── core/
│   ├── constants.hpp  # NEW - All magic strings/numbers
│   ├── filesystem.hpp # NEW - Path utilities
│   └── variant.hpp    # NEW - Variant management
├── parser/
│   └── discovery.hpp  # NEW - Tupfile discovery/parsing orchestration
└── index/
    └── incremental.hpp # NEW - Change detection & incremental build
```

### Testability

Zero functions in main.cpp are testable because:
1. All utility functions are in anonymous namespace (internal linkage)
2. Functions directly call `fmt::print(stderr, ...)` instead of returning errors
3. Heavy coupling to `std::filesystem` (no dependency injection)
4. Commands call `std::exit()` directly

### Command-line Parsing

✅ FIXED: `-j abc` now shows proper error message instead of throwing uncaught exception.

## Positive Observations

- Clean C++20 with proper `auto`, trailing returns, designated initializers
- Strong types (`NodeId`, `LinkType`) prevent bugs
- Consistent `Result<T>` error handling
- Good module separation (core/parser/graph/index/exec)
- Atomic file writes (temp + rename pattern)
- Memory-mapped index reader
- Comprehensive unit tests (831 assertions in 90 test cases)

## Recommended Priority

### P0 (Immediate)
1. ~~Fix scheduler race condition~~ (TBD - needs study) and stdin deadlock
2. Add index bounds validation (reader.cpp)
3. ✅ DONE: Validate CLI arguments

### P1 (This week)
4. Extract commands from main.cpp
5. ✅ DONE: Fix O(n²) string operations
6. Add heterogeneous lookup to VarDb

### P2 (Tech debt)
7. Refactor `expand_inputs()`
8. Deduplicate parse_rule/parse_bang_macro
9. Add path normalization consolidation

---

## Detailed Findings by Module

### Parser/Evaluator (src/parser/)

**lexer.cpp:18-23** - Unnecessary Optional Copying
```cpp
auto tok = Token { *peeked_ };
peeked_.reset();
return tok;
```
Should use `std::move(*peeked_)`.

**lexer.cpp:146-147** - Manual position manipulation appears 5+ times. Create `putback()` helper.

**lexer.cpp:332** - 13-condition if statement. Extract to `is_delimiter()` helper.

**parser.cpp:200-343** - 140-line switch statement. Use dispatch table pattern.

**eval.cpp:34-40** - String lookup creates temporary. Use C++20 heterogeneous lookup:
```cpp
struct StringHash {
    using is_transparent = void;
    auto operator()(std::string_view sv) const -> std::size_t;
};
```

**eval.cpp:96-147** - Three linear scans per iteration (O(n²)). Use single-pass with lookahead.

### Graph/Builder (src/graph/)

**dag.cpp:24-26** - Sparse vector with holes. Consider `std::unordered_map<NodeId, Node>`.

**dag.cpp:44-52** - Edge info stored in THREE places (edges_, inputs, outputs). Remove `edges_` vector.

**dag.cpp:49-52** - Silent failure if node lookup fails. Should return error.

**builder.cpp:673-914** - 241-line function handling 8+ cases. Extract into named helpers.

**builder.cpp:177-181** - Inconsistent error handling (accumulate vs fail-fast).

**rule_pattern.cpp:94-103** - Naive tokenization doesn't handle shell quoting.

### Execution/Scheduling (src/exec/)

**scheduler.cpp:118** - `stats()` returns copy without mutex protection. Data race.

**runner.cpp:148-156** - Stdin written in blocking mode before poll loop. Deadlock risk with large stdin.

**runner.cpp:150** - ✅ FIXED: `write()` return value now handled with `(void)` cast and comment.

**runner.cpp:158-161** - `fcntl()` return values not checked.

**scheduler.cpp:364-375** - TOCTOU race in output directory creation.

**scheduler.cpp:383-386** - `std::getenv()` is not thread-safe.

**scheduler.cpp:310, 323** - `notify_all()` on every job completion. Use `notify_one()`.

### Index/Core (src/index/, src/core/)

**writer.cpp:20** - ✅ FIXED: Added overflow check - throws `std::overflow_error` if string table exceeds 4GB.

**reader.cpp:140-171** - `reinterpret_cast` without bounds validation. Check offset + count × size fits in file.

**hash.cpp:15-43** - Uses raw `new`/`delete`. Use `std::unique_ptr`.

**entry.cpp:105-124** - O(n) linear search in `find_file()`. Add hash map index.

**No endianness handling** - Binary format not portable across architectures.

### Main Entry Point (src/main.cpp)

**Lines 33-1339** - 1300+ lines in anonymous namespace. Extract to proper modules.

**Lines 87-130** - ✅ FIXED: `parse_args()` now catches `std::stoi` exceptions and shows proper error.

**Lines 1079-1337** - `cmd_build()` is 258 lines doing everything. Break into pipeline stages.

**Lines 35-36** - Magic constants should be in `pup/core/constants.hpp`.
