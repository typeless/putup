# VarDb String Interning

## Goal

Replace `VarDb`'s `vector<Entry>` (where Entry = `{string name, string value}`) with `SortedPairVec` storing `(StringId, StringId)` pairs backed by a shared `StringPool*`. Eliminates per-entry heap allocations and makes VarDb copy O(n) on integer pairs instead of O(n × string length).

## Context

VarDb stores Tupfile variables (`CC=gcc`, `CFLAGS=-Wall -O2`, etc.). It is the most-copied structure during graph building — each Tupfile directory gets a fresh copy of `base_vars`. With ~75 Tupfiles and ~100 variables each, that's ~7,500 string copies per build. After interning, each copy becomes a `memcpy` of a flat integer array.

VarDb already uses a sorted vector with binary search (no STL associative containers). This change interns the string content, not the container structure.

## Design

### Internal representation

```cpp
class VarDb {
public:
    explicit VarDb(StringPool* pool);

    auto set(std::string_view name, std::string value) -> void;
    auto append(std::string_view name, std::string_view value) -> void;
    [[nodiscard]] auto get(std::string_view name) const -> std::string_view;
    [[nodiscard]] auto contains(std::string_view name) const -> bool;
    [[nodiscard]] auto names() const -> std::vector<std::string_view>;
    auto clear() -> void;

private:
    SortedPairVec entries_;  // StringId(name) → StringId(value)
    StringPool* pool_;       // borrowed, not owned
};
```

Public API is unchanged — `string_view` in/out. The `StringPool*` is the only new parameter.

### Operations

- **`set(name, value)`**: `pool_->intern(name)` → name_id, `pool_->intern(value)` → value_id, `entries_.insert(name_id, value_id)`. `SortedPairVec::insert` is insert-or-update: returns `false` when updating an existing key (the update path).
- **`get(name)`**: `pool_->find(name)` → name_id. If empty, return `""`. `entries_.find(name_id)` → value_id ptr. If null, return `""`. `pool_->get(value_id)` → string_view.
- **`contains(name)`**: `pool_->find(name)` → name_id. If empty, return false. `entries_.contains(name_id)`.
- **`append(name, value)`**: `pool_->intern(name)` → name_id once. Look up existing value_id via `entries_.find(name_id)`. If found, concatenate `pool_->get(*value_id) + " " + value`, intern result, update entry. If not found, intern value, insert new entry. Single name lookup, no double-find.
- **`names()`**: Iterate `entries_` via `for_each`, resolve each key through `pool_->get()`.
- **`clear()`**: `entries_.clear()`. Pool is shared — strings stay interned (harmless).

### Construction & copy

- **Constructor**: `VarDb(StringPool* pool)` — required, no default.
- **Copy**: Copies `SortedPairVec` (cheap flat integer array), shares `pool_` pointer.
- **Move**: Moves `SortedPairVec`, transfers `pool_` pointer.

### StringPool threading

All VarDbs in a build session share `Graph::strings`. The pool is accessed via a new `BuildGraph::string_pool()` accessor (returns `StringPool&`).

1. `BuildContext::Impl` is constructed — `graph` (a `BuildGraph`) is default-constructed, which creates `Graph` with its `StringPool strings`. The pool exists before any VarDb is created.
2. `parse_config()` and `parse_config_string()` receive `StringPool*` → return `VarDb` with entries interned.
3. `find_config_for_dir()` receives `StringPool*` (via `TupfileParseState` or direct parameter) for bare `VarDb{}` constructions.
4. Per-Tupfile `VarDb` is copy-constructed from `base_vars` → shares same pool.
5. `BuildContext::Impl::config_vars` and `vars` are initialized with the pool pointer after `Impl` construction (two-phase init, or lazy pointer assignment before first use).

Key invariant: **the pool outlives all VarDbs**.

### Test pattern

```cpp
auto pool = StringPool{};
auto db = VarDb{&pool};
db.set("CC", "gcc");
REQUIRE(db.get("CC") == "gcc");
```

Every test creates a local `StringPool` on the stack.

## Scope

### In scope
- VarDb internal representation change
- Threading `StringPool*` through `parse_config()`, `parse_config_string()`, and `find_config_for_dir()`
- Per-Tupfile VarDb construction in `context.cpp parse_directory()`
- All existing VarDb tests updated to pass `StringPool*`
- `BuildContext::Impl` wiring

### Out of scope
- Parser AST interning (deferred — AST is transient, not the bottleneck)
- BangMacroDef string interning (deferred — requires AST changes)
- BuilderOptions string fields (session-level constants, not worth interning)

## Files modified

| File | Change |
|------|--------|
| `include/pup/parser/eval.hpp` | VarDb internals: `SortedPairVec` + `StringPool*`, remove `vector<Entry>` |
| `src/parser/eval.cpp` | VarDb method implementations using interned StringIds |
| `include/pup/parser/config.hpp` | `parse_config()` and `parse_config_string()` signatures: add `StringPool*` parameter |
| `src/parser/config.cpp` | Forward `StringPool*` to VarDb construction |
| `include/pup/graph/dag.hpp` | Add `BuildGraph::string_pool()` accessor (returns `StringPool&`) |
| `src/cli/context.cpp` | Thread pool through `get_or_parse_config()`, `find_config_for_dir()`, `parse_directory()`, `BuildContext::Impl` members |
| `test/unit/test_eval.cpp` | Add `StringPool` to all VarDb tests |
| `test/unit/test_builder.cpp` | Add `StringPool` to builder tests using VarDb |

## Risks

- **Append growth**: `append()` interns concatenated results. Old values stay in pool (never freed). StringPool is append-only by design — this is consistent but means pool grows monotonically. For typical builds (~1000 variables, ~10 appends each), pool growth is negligible (~50KB).
- **Pool lifetime**: All VarDbs must not outlive the pool. Current architecture guarantees this — Graph (which owns the pool) is in `BuildContext::Impl` and outlives all parsing state.
- **Copy semantics change**: VarDb copy no longer deep-copies strings. This is the intended optimization but means two VarDb copies share string storage. Since strings are immutable in the pool, this is safe.

## Success criteria

- All 397+ tests pass
- VarDb copy is a flat integer array copy (no string allocations)
- No new `#include` of STL associative containers
- `names()` iteration order preserved (sorted by name)
- Binary size unchanged or decreased
