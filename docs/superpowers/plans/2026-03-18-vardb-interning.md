# VarDb String Interning Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace VarDb's `vector<Entry{string,string}>` with `SortedPairVec{StringId,StringId}` backed by a shared `StringPool*`, making VarDb copy a cheap integer-array memcpy.

**Architecture:** VarDb's internal storage changes from heap-allocated string pairs to interned integer pairs. A `StringPool*` (borrowed, not owned) is threaded from `Graph::strings` through config parsing and Tupfile evaluation. The public API (`string_view` in/out) is unchanged — only the constructor gains a `StringPool*` parameter.

**Tech Stack:** C++20, SortedPairVec (existing), StringPool (existing), Catch2 BDD tests, putup build system.

**Spec:** `docs/superpowers/specs/2026-03-18-vardb-interning-design.md`

**Build/test commands:**
```bash
make                                          # Build
./build/test/unit/putup_test '[eval]'         # VarDb + eval tests
./build/test/unit/putup_test '[builder]'      # Builder tests
./build/test/unit/putup_test '[e2e]'          # E2E tests (exercises full pipeline)
./build/test/unit/putup_test                  # All tests
make format                                   # clang-format
```

---

## File Map

| File | Role | Change |
|------|------|--------|
| `include/pup/parser/eval.hpp` | VarDb class definition | Replace `vector<Entry>` with `SortedPairVec` + `StringPool*` |
| `src/parser/eval.cpp` | VarDb method implementations | Rewrite set/get/contains/append/names/clear using interned ids |
| `include/pup/parser/config.hpp` | Config parser API | Add `StringPool*` parameter to `parse_config`, `parse_config_string` |
| `src/parser/config.cpp` | Config parser implementation | Forward `StringPool*` to VarDb constructor |
| `include/pup/graph/dag.hpp` | BuildGraph class | Add `string_pool()` accessor returning `StringPool&` |
| `src/cli/context.cpp` | Build orchestration | Thread pool through Impl, config loading, Tupfile parsing |
| `test/unit/test_eval.cpp` | VarDb + eval tests | Add `StringPool` to ~25 VarDb construction sites |
| `test/unit/test_builder.cpp` | Builder tests | Add `StringPool` to ~23 VarDb construction sites |

---

## Task 1: Add `BuildGraph::string_pool()` accessor

Small, zero-risk prerequisite. Exposes the pool pointer that all downstream code needs.

**Files:**
- Modify: `include/pup/graph/dag.hpp` (BuildGraph class, around line 640)

- [ ] **Step 1: Add accessor to BuildGraph**

In `include/pup/graph/dag.hpp`, inside the BuildGraph class (after the existing `str()` method around line 651), add:

```cpp
    [[nodiscard]]
    auto string_pool() -> StringPool&
    {
        return graph_.strings;
    }

    [[nodiscard]]
    auto string_pool() const -> StringPool const&
    {
        return graph_.strings;
    }
```

- [ ] **Step 2: Build to verify no breakage**

Run: `make`
Expected: Clean build, no errors.

- [ ] **Step 3: Commit**

```bash
git add include/pup/graph/dag.hpp
git commit -m "Add BuildGraph::string_pool() accessor for VarDb interning"
```

---

## Task 2: Rewrite VarDb internals

The core change. Replace `vector<Entry>` with `SortedPairVec` + `StringPool*`. Update tests first (TDD).

**Files:**
- Modify: `include/pup/parser/eval.hpp` (VarDb class, lines 23-48)
- Modify: `src/parser/eval.cpp` (VarDb methods, lines 22-112)
- Modify: `test/unit/test_eval.cpp` (all `VarDb {}` → `VarDb{&pool}`)

- [ ] **Step 1: Update test_eval.cpp — add StringPool to all VarDb constructions**

Every `auto db = VarDb {};` and `auto vars = VarDb {};` and `auto config_vars = VarDb {};` in `test/unit/test_eval.cpp` needs a `StringPool`. Add a pool at the top of each TEST_CASE or SECTION that creates a VarDb. The pattern:

```cpp
// Before:
auto db = VarDb {};

// After:
auto pool = StringPool {};
auto db = VarDb { &pool };
```

For tests with multiple VarDbs (e.g. `vars` + `config_vars`), they share the same pool:
```cpp
auto pool = StringPool {};
auto vars = VarDb { &pool };
auto config_vars = VarDb { &pool };
```

Add `#include "pup/core/string_pool.hpp"` at the top of `test_eval.cpp` if not already present.

Apply this mechanical transformation to all VarDb construction sites in test_eval.cpp (~23 sites).

- [ ] **Step 2: Run tests to verify they fail**

Run: `make`
Expected: FAIL — `VarDb` has no constructor taking `StringPool*`

- [ ] **Step 3: Rewrite VarDb class definition in eval.hpp**

Replace the VarDb class in `include/pup/parser/eval.hpp` (lines 23-48):

```cpp
class VarDb {
public:
    explicit VarDb(StringPool* pool);
    ~VarDb() = default;

    VarDb(VarDb const& other);
    auto operator=(VarDb const& other) -> VarDb&;

    VarDb(VarDb&&) noexcept = default;
    auto operator=(VarDb&&) noexcept -> VarDb& = default;

    auto set(std::string_view name, std::string value) -> void;
    auto append(std::string_view name, std::string_view value) -> void;

    [[nodiscard]]
    auto get(std::string_view name) const -> std::string_view;
    [[nodiscard]]
    auto contains(std::string_view name) const -> bool;
    [[nodiscard]]
    auto names() const -> std::vector<std::string_view>;

    auto clear() -> void;

    [[nodiscard]]
    auto pool() const -> StringPool* { return pool_; }

private:
    SortedPairVec entries_; // to_underlying(StringId) name → to_underlying(StringId) value
    StringPool* pool_;
};
```

Note: `SortedPairVec` is non-copyable (deleted copy ctor), so VarDb needs explicit copy ctor/assignment that reconstruct the entries. Add `#include "pup/core/sorted_id_vec.hpp"` to eval.hpp if not already present.

- [ ] **Step 4: Rewrite VarDb methods in eval.cpp**

Replace the VarDb implementation in `src/parser/eval.cpp` (lines 22-112). Remove `find_entry()`, `Entry` struct, and the old `entries_` vector.

**Important:** `SortedPairVec::for_each` takes a C function pointer `void(*)(uint32_t, uint32_t, void*)` — lambdas that capture variables cannot be used. Pass context through the `void* ctx` parameter.

```cpp
VarDb::VarDb(StringPool* pool)
    : pool_(pool)
{
}

VarDb::VarDb(VarDb const& other)
    : pool_(other.pool_)
{
    // SortedPairVec is non-copyable; rebuild by iterating source entries.
    // Source is sorted by StringId, so each insert appends at end (no shifting).
    other.entries_.for_each([](std::uint32_t key, std::uint32_t value, void* raw) {
        static_cast<SortedPairVec*>(raw)->insert(key, value);
    }, &entries_);
}

auto VarDb::operator=(VarDb const& other) -> VarDb&
{
    if (this != &other) {
        entries_.clear();
        pool_ = other.pool_;
        other.entries_.for_each([](std::uint32_t key, std::uint32_t value, void* raw) {
            static_cast<SortedPairVec*>(raw)->insert(key, value);
        }, &entries_);
    }
    return *this;
}

auto VarDb::set(std::string_view name, std::string value) -> void
{
    auto name_id = to_underlying(pool_->intern(name));
    auto value_id = to_underlying(pool_->intern(value));
    entries_.insert(name_id, value_id);
}

auto VarDb::append(std::string_view name, std::string_view value) -> void
{
    auto name_id = to_underlying(pool_->intern(name));
    auto const* existing = entries_.find(name_id);
    if (existing) {
        auto old_value = pool_->get(make_string_id(*existing));
        auto combined = std::string {};
        if (!old_value.empty()) {
            combined.reserve(old_value.size() + 1 + value.size());
            combined += old_value;
            combined += ' ';
        }
        combined += value;
        auto value_id = to_underlying(pool_->intern(combined));
        entries_.insert(name_id, value_id);
    } else {
        auto value_id = to_underlying(pool_->intern(value));
        entries_.insert(name_id, value_id);
    }
}

auto VarDb::get(std::string_view name) const -> std::string_view
{
    auto name_id = pool_->find(name);
    if (is_empty(name_id)) {
        return {};
    }
    auto const* value_ptr = entries_.find(to_underlying(name_id));
    if (!value_ptr) {
        return {};
    }
    return pool_->get(make_string_id(*value_ptr));
}

auto VarDb::contains(std::string_view name) const -> bool
{
    auto name_id = pool_->find(name);
    if (is_empty(name_id)) {
        return false;
    }
    return entries_.contains(to_underlying(name_id));
}

auto VarDb::names() const -> std::vector<std::string_view>
{
    // Collect names via for_each (iterated in StringId order, NOT lexicographic).
    // Sort the result to preserve the alphabetical ordering contract.
    struct Ctx {
        std::vector<std::string_view>* result;
        StringPool const* pool;
    };
    auto result = std::vector<std::string_view> {};
    auto ctx = Ctx { &result, pool_ };
    entries_.for_each([](std::uint32_t key, std::uint32_t, void* raw) {
        auto* c = static_cast<Ctx*>(raw);
        c->result->push_back(c->pool->get(make_string_id(key)));
    }, &ctx);
    std::sort(result.begin(), result.end());
    return result;
}

auto VarDb::clear() -> void
{
    entries_.clear();
}
```

- [ ] **Step 5: Build and run eval tests**

Run: `make && ./build/test/unit/putup_test '[eval]'`
Expected: All eval tests pass.

- [ ] **Step 6: Commit**

```bash
git add include/pup/parser/eval.hpp src/parser/eval.cpp test/unit/test_eval.cpp
git commit -m "Rewrite VarDb internals: SortedPairVec<StringId,StringId> + StringPool*

Replaces vector<Entry{string,string}> with interned integer pairs.
VarDb copy is now a flat integer array memcpy instead of N string copies.
Public API unchanged (string_view in/out)."
```

---

## Task 3: Thread StringPool through config parser and context.cpp

Update config parser signatures and wire `Graph::strings` to all VarDb construction sites. Done as a single task to avoid a broken intermediate state.

**Files:**
- Modify: `include/pup/parser/config.hpp` (lines 23-28)
- Modify: `src/parser/config.cpp` (lines 57-112)
- Modify: `src/cli/context.cpp` (multiple locations)

- [ ] **Step 1: Update config.hpp signatures**

```cpp
[[nodiscard]]
auto parse_config(std::string const& path, StringPool& pool) -> Result<VarDb>;

[[nodiscard]]
auto parse_config_string(std::string_view content, StringPool& pool) -> Result<VarDb>;
```

Add `#include "pup/core/string_pool.hpp"` to config.hpp.

- [ ] **Step 2: Update config.cpp implementations**

In `parse_config_string`: change `auto db = VarDb {};` → `auto db = VarDb { &pool };`

In `parse_config`: forward the pool → `return parse_config_string(*content, pool);`

- [ ] **Step 3: Add StringPool* to TupfileParseState**

In `context.cpp`, the `TupfileParseState` struct (around line 122) needs a pool pointer:

```cpp
struct TupfileParseState {
    // ... existing fields ...
    StringPool* pool = nullptr; // shared pool for all VarDbs
};
```

- [ ] **Step 4: Fix BuildContext::Impl member ordering**

`BuildContext::Impl` (line 581) currently has:
```cpp
parser::VarDb config_vars;
parser::VarDb vars;
graph::BuildGraph graph;
```

Move `graph` before the VarDbs so the pool exists before VarDb construction:
```cpp
graph::BuildGraph graph;
parser::VarDb config_vars;
parser::VarDb vars;
```

Then change `config_vars` and `vars` to use the pool:
```cpp
graph::BuildGraph graph;
parser::VarDb config_vars { &graph.string_pool() };
parser::VarDb vars { &graph.string_pool() };
```

- [ ] **Step 5: Fix get_or_parse_config**

`get_or_parse_config` (around line 244) calls `parse_config()`. Thread pool from `state.pool`:

```cpp
auto result = parser::parse_config(path, *state.pool);
```

- [ ] **Step 6: Fix find_config_for_dir**

Two bare `VarDb {}` constructions need the pool:

Line ~309: `state.scoped_configs.emplace_back(normalized, parser::VarDb { state.pool });`
Line ~315: `auto merged = parser::VarDb { state.pool };`

- [ ] **Step 7: Fix parse_directory**

Line ~361: `auto vars = pup::parser::VarDb { ctx.base_vars };` — this is a copy, which already works (copy ctor shares pool).

- [ ] **Step 8: Wire pool in build_context**

In `build_context()` (around line 637), after creating ctx, set the pool on state:
```cpp
ctx.impl_->state.pool = &ctx.impl_->graph.string_pool();
```

Fix the config_vars assignment (around line 692):
```cpp
ctx.impl_->config_vars = *root_cfg;
```
This is a copy-assignment — works because both VarDbs share the same pool.

- [ ] **Step 9: Build and run all tests**

Run: `make && ./build/test/unit/putup_test`
Expected: All 397+ tests pass.

- [ ] **Step 10: Run self-host parse test**

Run: `./build/putup parse -v`
Expected: Parses successfully with no segfaults (exercises the full config → parse → evaluate pipeline).

- [ ] **Step 11: Commit**

```bash
git add include/pup/parser/config.hpp src/parser/config.cpp src/cli/context.cpp
git commit -m "Thread StringPool through config parsing and build context

parse_config/parse_config_string take StringPool& parameter.
BuildContext::Impl reordered so graph (owning pool) initializes
before VarDb members. TupfileParseState carries pool pointer for
config cache VarDb construction."
```

---

## Task 4: Update builder tests

Mechanical transformation: add `StringPool` to all VarDb construction sites in test_builder.cpp.

**Files:**
- Modify: `test/unit/test_builder.cpp` (~25 VarDb construction sites)

- [ ] **Step 1: Add StringPool to all VarDb constructions in test_builder.cpp**

Add `#include "pup/core/string_pool.hpp"` if not present.

Same pattern as Task 2 Step 1. In each test that creates `auto vars = VarDb {};`, add a pool. Many builder tests create VarDb + EvalContext together. Pattern:

```cpp
// Before:
auto vars = VarDb {};
auto eval_ctx = EvalContext { .vars = &vars, ... };

// After:
auto pool = StringPool {};
auto vars = VarDb { &pool };
auto eval_ctx = EvalContext { .vars = &vars, ... };
```

Apply to all ~23 sites.

- [ ] **Step 2: Build and run builder tests**

Run: `make && ./build/test/unit/putup_test '[builder]'`
Expected: All builder tests pass.

- [ ] **Step 3: Run full test suite + E2E**

Run: `./build/test/unit/putup_test`
Expected: All 397+ tests pass.

- [ ] **Step 4: Run format**

Run: `make format`

- [ ] **Step 5: Commit**

```bash
git add test/unit/test_builder.cpp
git commit -m "Update builder tests for VarDb(StringPool*) constructor"
```

---

## Task 5: Final verification

**Files:** None (verification only)

- [ ] **Step 1: Clean build**

Run: `make clean && make`
Expected: Full clean build succeeds.

- [ ] **Step 2: Full test suite**

Run: `./build/test/unit/putup_test`
Expected: All 397+ tests pass.

- [ ] **Step 3: Self-host build test**

Run: `./build/putup -B build`
Expected: putup can build itself.

- [ ] **Step 4: Binary size check**

Run: `size build/putup`
Record `.text` size. Expected: same or smaller than previous (904 KB baseline).

- [ ] **Step 5: Verify no STL regression**

Run: `grep -rn '#include <unordered_map>\|#include <unordered_set>\|#include <set>\|#include <map>' src/ include/`
Expected: Zero matches.

---

## Execution Notes

**SortedPairVec copy workaround:** `SortedPairVec` has deleted copy constructor (uses `malloc`/`realloc`). VarDb's copy constructor must iterate the source via `for_each` and `insert` each pair into a fresh `SortedPairVec`. This is O(n) — one `insert` per entry — but since entries are iterated in sorted order, each insert appends at the end (no shifting), making it effectively O(n).

**`set()` value parameter:** The current API is `set(string_view name, string value)`. The `string value` parameter is moved into storage. After interning, we only need `string_view` for both parameters (the pool copies the content). However, keeping the `string value` signature avoids changing all callers — the `string` is implicitly convertible from `string_view` and the intern call takes a `string_view` anyway. Consider simplifying to `set(string_view, string_view)` if callers don't depend on the `string` parameter.

**Member ordering in Impl:** Moving `graph` before `config_vars`/`vars` in `BuildContext::Impl` is critical — C++ initializes members in declaration order, and the VarDb initializers reference `graph.string_pool()`.
