# Custom Containers Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace `std::unordered_map`, `std::unordered_set`, and `std::set` with tailored integer-keyed primitives backed by pervasive string interning.

**Architecture:** All strings are interned into StringId handles via StringPool. All containers become flat arrays (dense IDs), bitsets (set membership), sorted integer arrays (small sets), or arena slices (variable-length lists). The only hash table in the system is StringPool's internal Robin Hood index. Directory-name lookups use per-directory SortedPairVec arrays. NodeIds have flag bits in the high nibble, so IdArray dispatches across 4 per-type sub-arrays.

**Tech Stack:** C++20, POSIX/Win32, Catch2 BDD tests, putup build system (Tupfiles).

**Spec:** `docs/plans/custom-containers.md`

---

## Chunk 1: Primitives

### Task 1: IdBitSet

**Files:**
- Create: `include/pup/core/id_bitset.hpp`
- Create: `src/core/id_bitset.cpp`
- Create: `test/unit/test_id_bitset.cpp`
- Modify: `Tupfile` — add `src/core/id_bitset.cpp`
- Modify: `test/unit/Tupfile` — add `test_id_bitset.cpp`

- [ ] **Step 1: Write test**

```cpp
// test/unit/test_id_bitset.cpp
#include "catch_amalgamated.hpp"
#include "pup/core/id_bitset.hpp"

using pup::IdBitSet;

TEST_CASE("IdBitSet basic operations", "[id_bitset]")
{
    auto bs = IdBitSet {};

    SECTION("empty bitset")
    {
        REQUIRE(bs.count() == 0);
        REQUIRE_FALSE(bs.contains(1));
    }

    SECTION("insert and contains")
    {
        bs.resize(100);
        bs.insert(42);
        REQUIRE(bs.contains(42));
        REQUIRE_FALSE(bs.contains(41));
        REQUIRE(bs.count() == 1);
    }

    SECTION("remove")
    {
        bs.resize(100);
        bs.insert(10);
        bs.remove(10);
        REQUIRE_FALSE(bs.contains(10));
        REQUIRE(bs.count() == 0);
    }

    SECTION("clear")
    {
        bs.resize(100);
        bs.insert(1);
        bs.insert(50);
        bs.insert(99);
        bs.clear();
        REQUIRE(bs.count() == 0);
    }

    SECTION("for_each iterates set bits")
    {
        bs.resize(200);
        bs.insert(3);
        bs.insert(100);
        bs.insert(199);
        auto collected = std::vector<uint32_t> {};
        bs.for_each([&](uint32_t id) { collected.push_back(id); });
        REQUIRE(collected == std::vector<uint32_t> { 3, 100, 199 });
    }

    SECTION("duplicate insert is idempotent")
    {
        bs.resize(10);
        bs.insert(5);
        bs.insert(5);
        REQUIRE(bs.count() == 1);
    }

    SECTION("boundary: id 0")
    {
        bs.resize(1);
        bs.insert(0);
        REQUIRE(bs.contains(0));
    }

    SECTION("boundary: id at word boundary")
    {
        bs.resize(128);
        bs.insert(63);
        bs.insert(64);
        REQUIRE(bs.contains(63));
        REQUIRE(bs.contains(64));
        REQUIRE_FALSE(bs.contains(62));
        REQUIRE_FALSE(bs.contains(65));
    }
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `make && ./build/test/unit/putup_test "[id_bitset]" -v`
Expected: FAIL — `id_bitset.hpp` not found

- [ ] **Step 3: Implement IdBitSet**

```cpp
// include/pup/core/id_bitset.hpp
#pragma once

#include <cstdint>
#include <functional>

namespace pup {

class IdBitSet {
public:
    IdBitSet() = default;
    ~IdBitSet();

    IdBitSet(IdBitSet const&) = delete;
    auto operator=(IdBitSet const&) -> IdBitSet& = delete;
    IdBitSet(IdBitSet&&) noexcept;
    auto operator=(IdBitSet&&) noexcept -> IdBitSet&;

    auto resize(std::uint32_t max_id) -> void;
    auto insert(std::uint32_t id) -> void;
    auto remove(std::uint32_t id) -> void;
    [[nodiscard]] auto contains(std::uint32_t id) const -> bool;
    auto clear() -> void;
    [[nodiscard]] auto count() const -> std::size_t;
    auto for_each(std::function<void(std::uint32_t)> const& fn) const -> void;

private:
    std::uint64_t* words_ = nullptr;
    std::size_t word_count_ = 0;
};

} // namespace pup
```

```cpp
// src/core/id_bitset.cpp
#include "pup/core/id_bitset.hpp"
#include <cstdlib>
#include <cstring>

namespace pup {

IdBitSet::~IdBitSet() { std::free(words_); }

IdBitSet::IdBitSet(IdBitSet&& o) noexcept
    : words_(o.words_), word_count_(o.word_count_)
{
    o.words_ = nullptr;
    o.word_count_ = 0;
}

auto IdBitSet::operator=(IdBitSet&& o) noexcept -> IdBitSet&
{
    if (this != &o) {
        std::free(words_);
        words_ = o.words_;
        word_count_ = o.word_count_;
        o.words_ = nullptr;
        o.word_count_ = 0;
    }
    return *this;
}

auto IdBitSet::resize(std::uint32_t max_id) -> void
{
    auto needed = static_cast<std::size_t>((max_id + 64) / 64);
    if (needed <= word_count_) return;
    words_ = static_cast<std::uint64_t*>(std::realloc(words_, needed * sizeof(std::uint64_t)));
    std::memset(words_ + word_count_, 0, (needed - word_count_) * sizeof(std::uint64_t));
    word_count_ = needed;
}

auto IdBitSet::insert(std::uint32_t id) -> void
{
    auto w = id / 64;
    if (w >= word_count_) resize(id);
    words_[w] |= std::uint64_t { 1 } << (id % 64);
}

auto IdBitSet::remove(std::uint32_t id) -> void
{
    auto w = id / 64;
    if (w < word_count_) {
        words_[w] &= ~(std::uint64_t { 1 } << (id % 64));
    }
}

auto IdBitSet::contains(std::uint32_t id) const -> bool
{
    auto w = id / 64;
    if (w >= word_count_) return false;
    return (words_[w] & (std::uint64_t { 1 } << (id % 64))) != 0;
}

auto IdBitSet::clear() -> void
{
    if (words_) std::memset(words_, 0, word_count_ * sizeof(std::uint64_t));
}

auto IdBitSet::count() const -> std::size_t
{
    auto n = std::size_t { 0 };
    for (std::size_t i = 0; i < word_count_; ++i) {
        n += static_cast<std::size_t>(__builtin_popcountll(words_[i]));
    }
    return n;
}

auto IdBitSet::for_each(std::function<void(std::uint32_t)> const& fn) const -> void
{
    for (std::size_t w = 0; w < word_count_; ++w) {
        auto bits = words_[w];
        while (bits) {
            auto bit = static_cast<std::uint32_t>(__builtin_ctzll(bits));
            fn(static_cast<std::uint32_t>(w * 64) + bit);
            bits &= bits - 1;
        }
    }
}

} // namespace pup
```

- [ ] **Step 4: Add to Tupfiles**

In `Tupfile` after `srcs-y += src/core/hash.cpp`:
```
srcs-y += src/core/id_bitset.cpp
```

In `test/unit/Tupfile` after `test-srcs-y += test_hash.cpp`:
```
test-srcs-y += test_id_bitset.cpp
```

- [ ] **Step 5: Run test to verify it passes**

Run: `make && ./build/test/unit/putup_test "[id_bitset]" -v`
Expected: All sections PASS

- [ ] **Step 6: Run full test suite**

Run: `make test`
Expected: All 349+ tests pass (no regressions)

- [ ] **Step 7: Commit**

```bash
git add include/pup/core/id_bitset.hpp src/core/id_bitset.cpp \
        test/unit/test_id_bitset.cpp Tupfile test/unit/Tupfile
git commit -m "Add IdBitSet primitive for dense ID set membership"
```

---

### Task 2: IdArray32 and IdArray64

**Files:**
- Create: `include/pup/core/id_array.hpp`
- Create: `src/core/id_array.cpp`
- Create: `test/unit/test_id_array.cpp`
- Modify: `Tupfile` — add `src/core/id_array.cpp`
- Modify: `test/unit/Tupfile` — add `test_id_array.cpp`

- [ ] **Step 1: Write test**

```cpp
// test/unit/test_id_array.cpp
#include "catch_amalgamated.hpp"
#include "pup/core/id_array.hpp"

using pup::IdArray32;
using pup::IdArray64;

TEST_CASE("IdArray32 basic operations", "[id_array]")
{
    auto arr = IdArray32 {};

    SECTION("empty array")
    {
        REQUIRE_FALSE(arr.contains(1));
    }

    SECTION("set and get")
    {
        arr.resize(100);
        arr.set(42, 0xDEAD);
        REQUIRE(arr.contains(42));
        REQUIRE(arr.get(42) == 0xDEAD);
    }

    SECTION("unset slot returns 0")
    {
        arr.resize(10);
        REQUIRE(arr.get(5) == 0);
    }

    SECTION("clear resets all")
    {
        arr.resize(10);
        arr.set(3, 100);
        arr.set(7, 200);
        arr.clear();
        REQUIRE_FALSE(arr.contains(3));
        REQUIRE_FALSE(arr.contains(7));
    }

    SECTION("for_each iterates occupied slots")
    {
        arr.resize(100);
        arr.set(10, 1);
        arr.set(50, 2);
        auto sum = std::uint32_t { 0 };
        arr.for_each([&](std::uint32_t id, std::uint32_t val) { sum += val; });
        REQUIRE(sum == 3);
    }

    SECTION("set with value 0 is still present")
    {
        arr.resize(10);
        arr.set(5, 0);
        REQUIRE(arr.contains(5));
    }
}

TEST_CASE("IdArray64 stores 64-bit values", "[id_array]")
{
    auto arr = IdArray64 {};
    arr.resize(10);
    arr.set(3, 0xDEADBEEF'CAFEBABE);
    REQUIRE(arr.get(3) == 0xDEADBEEF'CAFEBABE);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `make && ./build/test/unit/putup_test "[id_array]" -v`
Expected: FAIL — `id_array.hpp` not found

- [ ] **Step 3: Implement IdArray32 and IdArray64**

`IdArray32`/`IdArray64` are flat `malloc`'d arrays with a parallel `IdBitSet` for presence tracking. Implementation in `.cpp` file. The header declares both concrete types (no template).

Key design: `set(id, value)` automatically marks the bit in the parallel bitset. `contains(id)` checks the bitset (not the value — so value=0 is distinguishable from "not set").

```cpp
// include/pup/core/id_array.hpp
#pragma once

#include "pup/core/id_bitset.hpp"
#include <cstdint>
#include <functional>

namespace pup {

class IdArray32 {
public:
    IdArray32() = default;
    ~IdArray32();
    IdArray32(IdArray32&&) noexcept;
    auto operator=(IdArray32&&) noexcept -> IdArray32&;
    IdArray32(IdArray32 const&) = delete;
    auto operator=(IdArray32 const&) -> IdArray32& = delete;

    auto resize(std::uint32_t max_id) -> void;
    auto set(std::uint32_t id, std::uint32_t value) -> void;
    [[nodiscard]] auto get(std::uint32_t id) const -> std::uint32_t;
    [[nodiscard]] auto contains(std::uint32_t id) const -> bool;
    auto clear() -> void;
    auto for_each(std::function<void(std::uint32_t id, std::uint32_t value)> const& fn) const -> void;

private:
    std::uint32_t* data_ = nullptr;
    std::size_t capacity_ = 0;
    IdBitSet present_;
};

class IdArray64 {
public:
    IdArray64() = default;
    ~IdArray64();
    IdArray64(IdArray64&&) noexcept;
    auto operator=(IdArray64&&) noexcept -> IdArray64&;
    IdArray64(IdArray64 const&) = delete;
    auto operator=(IdArray64 const&) -> IdArray64& = delete;

    auto resize(std::uint32_t max_id) -> void;
    auto set(std::uint32_t id, std::uint64_t value) -> void;
    [[nodiscard]] auto get(std::uint32_t id) const -> std::uint64_t;
    [[nodiscard]] auto contains(std::uint32_t id) const -> bool;
    auto clear() -> void;
    auto for_each(std::function<void(std::uint32_t id, std::uint64_t value)> const& fn) const -> void;

private:
    std::uint64_t* data_ = nullptr;
    std::size_t capacity_ = 0;
    IdBitSet present_;
};

} // namespace pup
```

Implementation in `src/core/id_array.cpp`: `malloc`/`realloc`/`free` for data array, delegates presence to `IdBitSet`. `set()` calls `present_.insert(id)`. `contains()` calls `present_.contains(id)`. `for_each()` calls `present_.for_each()` and reads `data_[id]`.

- [ ] **Step 4: Add to Tupfiles, build, test**

Run: `make && ./build/test/unit/putup_test "[id_array]" -v`
Expected: All sections PASS

- [ ] **Step 5: Run full suite, commit**

Run: `make test`

```bash
git add include/pup/core/id_array.hpp src/core/id_array.cpp \
        test/unit/test_id_array.cpp Tupfile test/unit/Tupfile
git commit -m "Add IdArray32/IdArray64 for dense ID-indexed storage"
```

---

### Task 3: Arena32

**Files:**
- Create: `include/pup/core/arena.hpp`
- Create: `src/core/arena.cpp`
- Create: `test/unit/test_arena.cpp`
- Modify: `Tupfile`, `test/unit/Tupfile`

- [ ] **Step 1: Write test**

```cpp
// test/unit/test_arena.cpp
#include "catch_amalgamated.hpp"
#include "pup/core/arena.hpp"

using pup::Arena32;
using pup::ArenaSlice;

TEST_CASE("Arena32 basic operations", "[arena]")
{
    auto arena = Arena32 {};

    SECTION("empty arena")
    {
        REQUIRE(arena.size() == 0);
    }

    SECTION("append and get")
    {
        std::uint32_t vals[] = { 10, 20, 30 };
        auto slice = arena.append(vals, 3);
        REQUIRE(slice.length == 3);
        auto span = arena.get(slice);
        REQUIRE(span[0] == 10);
        REQUIRE(span[1] == 20);
        REQUIRE(span[2] == 30);
    }

    SECTION("multiple appends are contiguous")
    {
        std::uint32_t a[] = { 1, 2 };
        std::uint32_t b[] = { 3, 4, 5 };
        auto sa = arena.append(a, 2);
        auto sb = arena.append(b, 3);
        REQUIRE(sb.offset == sa.offset + sa.length);
        REQUIRE(arena.size() == 5);
    }

    SECTION("empty append")
    {
        auto slice = arena.append(nullptr, 0);
        REQUIRE(slice.length == 0);
    }

    SECTION("compact shrinks to fit")
    {
        arena.reserve(1000);
        std::uint32_t v[] = { 42 };
        arena.append(v, 1);
        arena.compact();
        REQUIRE(arena.size() == 1);
        REQUIRE(arena.get(ArenaSlice { 0, 1 })[0] == 42);
    }
}
```

- [ ] **Step 2: Run test, verify fail**
- [ ] **Step 3: Implement Arena32**

`ArenaSlice` is a simple struct `{uint32_t offset, uint32_t length}`. `Arena32` owns a `malloc`'d `uint32_t` buffer. `append()` copies values and returns the slice. `get()` returns a pointer+length. `reserve()` pre-allocates. `compact()` reallocs to exact size.

- [ ] **Step 4: Add to Tupfiles, build, test**
- [ ] **Step 5: Full suite, commit**

```bash
git commit -m "Add Arena32 for append-only variable-length integer lists"
```

---

### Task 4: SortedIdVec and SortedPairVec

**Files:**
- Create: `include/pup/core/sorted_id_vec.hpp`
- Create: `src/core/sorted_id_vec.cpp`
- Create: `test/unit/test_sorted_id_vec.cpp`
- Modify: `Tupfile`, `test/unit/Tupfile`

- [ ] **Step 1: Write test**

```cpp
// test/unit/test_sorted_id_vec.cpp
#include "catch_amalgamated.hpp"
#include "pup/core/sorted_id_vec.hpp"

using pup::SortedIdVec;
using pup::SortedPairVec;

TEST_CASE("SortedIdVec basic operations", "[sorted_id_vec]")
{
    auto vec = SortedIdVec {};

    SECTION("insert maintains sorted order")
    {
        vec.insert(30);
        vec.insert(10);
        vec.insert(20);
        REQUIRE(vec.size() == 3);
        auto collected = std::vector<std::uint32_t> {};
        vec.for_each([&](std::uint32_t id) { collected.push_back(id); });
        REQUIRE(collected == std::vector<std::uint32_t> { 10, 20, 30 });
    }

    SECTION("duplicate insert is idempotent")
    {
        vec.insert(5);
        vec.insert(5);
        REQUIRE(vec.size() == 1);
    }

    SECTION("contains")
    {
        vec.insert(42);
        REQUIRE(vec.contains(42));
        REQUIRE_FALSE(vec.contains(41));
    }

    SECTION("remove")
    {
        vec.insert(1);
        vec.insert(2);
        vec.insert(3);
        vec.remove(2);
        REQUIRE(vec.size() == 2);
        REQUIRE_FALSE(vec.contains(2));
        REQUIRE(vec.contains(1));
        REQUIRE(vec.contains(3));
    }
}

TEST_CASE("SortedPairVec basic operations", "[sorted_id_vec]")
{
    auto vec = SortedPairVec {};

    SECTION("insert and find")
    {
        vec.insert(10, 100);
        vec.insert(5, 50);
        vec.insert(15, 150);
        REQUIRE(vec.find(10) != nullptr);
        REQUIRE(*vec.find(10) == 100);
        REQUIRE(*vec.find(5) == 50);
        REQUIRE(vec.find(99) == nullptr);
    }

    SECTION("insert overwrites existing key")
    {
        vec.insert(10, 100);
        vec.insert(10, 200);
        REQUIRE(vec.size() == 1);
        REQUIRE(*vec.find(10) == 200);
    }

    SECTION("remove")
    {
        vec.insert(1, 10);
        vec.insert(2, 20);
        vec.remove(1);
        REQUIRE(vec.find(1) == nullptr);
        REQUIRE(*vec.find(2) == 20);
    }
}
```

- [ ] **Step 2: Run test, verify fail**
- [ ] **Step 3: Implement**

`SortedIdVec`: `malloc`'d `uint32_t` array, hand-written binary search. Insert uses `memmove` to maintain order.

`SortedPairVec`: `malloc`'d array of `{uint32_t key, uint32_t value}` pairs, sorted by key. Binary search on key. `find()` returns pointer to value or `nullptr`.

- [ ] **Step 4: Build, test, full suite, commit**

```bash
git commit -m "Add SortedIdVec and SortedPairVec for small sorted integer sets"
```

---

### Task 5: StringPool Robin Hood index

**Files:**
- Modify: `include/pup/core/string_pool.hpp`
- Modify: `src/core/string_pool.cpp`
- Modify: `test/unit/test_string_pool.cpp` (if exists, else the existing tests cover it via other test files)

- [ ] **Step 1: Write stress test for Robin Hood index**

Add to existing string pool tests (or create `test_string_pool.cpp`):

```cpp
TEST_CASE("StringPool Robin Hood index", "[string_pool]")
{
    auto pool = pup::StringPool {};

    SECTION("intern and find 10K strings")
    {
        for (auto i = 0; i < 10000; ++i) {
            auto s = "var_" + std::to_string(i);
            auto id = pool.intern(s);
            REQUIRE(pool.get(id) == s);
            REQUIRE(pool.find(s) == id);
        }
        REQUIRE(pool.size() == 10000);
    }

    SECTION("deduplication under load")
    {
        for (auto i = 0; i < 1000; ++i) {
            pool.intern("same_string");
        }
        REQUIRE(pool.size() == 1);
    }
}
```

- [ ] **Step 2: Run existing tests to establish baseline**

Run: `make test`
Expected: All pass (baseline)

- [ ] **Step 3: Replace unordered_map index with Robin Hood**

In `string_pool.cpp`, replace `std::unordered_map<std::string_view, StringId>` with an internal Robin Hood hash table. The Robin Hood table:

- Uses FNV-1a hash on string bytes
- Stores `{uint32_t hash, uint16_t displacement}` metadata per slot
- Stores `StringId` values per slot (the string_view key is reconstructed from `storage_[to_underlying(id) - 1]`)
- Key comparison: compute FNV-1a of probe key, compare hash first, then compare string content via `storage_` lookup
- Sentinel: hash=0 means empty, hash=1 means tombstone. `fix_hash()` remaps real hashes of 0/1 to 2/3.
- Growth: double at 80% load factor
- No `<unordered_map>` include needed

In `string_pool.hpp`, remove `#include <unordered_map>`. Replace private member with:
```cpp
private:
    std::deque<std::string> storage_;
    // Robin Hood index (inline — no separate type)
    struct Meta { std::uint32_t hash; std::uint16_t displacement; };
    Meta* index_meta_ = nullptr;
    StringId* index_values_ = nullptr;
    std::size_t index_capacity_ = 0;
    std::size_t index_count_ = 0;
```

- [ ] **Step 4: Run tests**

Run: `make && make test`
Expected: All tests pass (including the new stress test)

- [ ] **Step 5: Commit**

```bash
git commit -m "Replace StringPool unordered_map with Robin Hood index

The only hash table in the system — specialized for string_view
keys with FNV-1a hash. Removes <unordered_map> from string_pool.hpp."
```

---

## Chunk 2: Graph Migration

### Task 6: NodeIdMap wrapper

**Files:**
- Create: `include/pup/core/node_id_map.hpp`

NodeIdMap provides a single `get(NodeId)`/`set(NodeId, value)` API over 4 per-type IdArrays, dispatching on `node_id::is_command()`, `is_condition()`, `is_phi()`, else file.

- [ ] **Step 1: Write test**

```cpp
// test/unit/test_node_id_map.cpp
#include "catch_amalgamated.hpp"
#include "pup/core/node_id_map.hpp"
#include "pup/core/types.hpp"

using pup::NodeIdMap32;

TEST_CASE("NodeIdMap32 dispatches by node type", "[node_id_map]")
{
    auto map = NodeIdMap32 {};
    map.resize_files(100);
    map.resize_commands(50);

    auto file_id = pup::NodeId { 5 };
    auto cmd_id = pup::node_id::make_command(3);

    SECTION("file and command slots are independent")
    {
        map.set(file_id, 111);
        map.set(cmd_id, 222);
        REQUIRE(map.get(file_id) == 111);
        REQUIRE(map.get(cmd_id) == 222);
    }

    SECTION("contains checks correct sub-array")
    {
        map.set(file_id, 1);
        REQUIRE(map.contains(file_id));
        REQUIRE_FALSE(map.contains(cmd_id));
    }
}
```

- [ ] **Step 2: Implement NodeIdMap32/NodeIdMap64**

Header-only wrapper. Holds 4 `IdArray32` (or `IdArray64`) members. `set()`/`get()`/`contains()` dispatch on `node_id::is_command()` etc., then call `sub_array.set(node_id::index(id), value)`.

```cpp
// include/pup/core/node_id_map.hpp
#pragma once
#include "pup/core/id_array.hpp"
#include "pup/core/types.hpp"

namespace pup {

class NodeIdMap32 {
public:
    auto resize_files(std::uint32_t max_idx) -> void { files_.resize(max_idx); }
    auto resize_commands(std::uint32_t max_idx) -> void { cmds_.resize(max_idx); }
    auto resize_conditions(std::uint32_t max_idx) -> void { conds_.resize(max_idx); }
    auto resize_phis(std::uint32_t max_idx) -> void { phis_.resize(max_idx); }

    auto set(NodeId id, std::uint32_t value) -> void;
    [[nodiscard]] auto get(NodeId id) const -> std::uint32_t;
    [[nodiscard]] auto contains(NodeId id) const -> bool;
    auto clear() -> void;

private:
    IdArray32 files_, cmds_, conds_, phis_;
};

// Same pattern for NodeIdMap64

} // namespace pup
```

Implementation dispatches on `node_id::is_command(id)` etc.

- [ ] **Step 3: Build, test, commit**

```bash
git commit -m "Add NodeIdMap32/64 wrapper dispatching by node type flags"
```

---

### Task 7: Migrate Graph edge indices

**Files:**
- Modify: `include/pup/graph/dag.hpp` — replace 4 edge index `unordered_map`s with `NodeIdMap64` + `Arena32`
- Modify: `src/graph/dag.cpp` — update `add_edge()`, `build_edge_indices()`, edge lookup functions, `get_inputs()`, `get_outputs()`, `edges_to()`, `edges_from()`
- Modify: `src/graph/builder.cpp` — update callers of edge lookup functions
- Modify: `src/graph/topo.cpp` — update callers of edge traversal
- Modify: `src/exec/scheduler.cpp` — update job dependency resolution
- Modify: `src/cli/cmd_build.cpp` — update upstream node walk
- Modify: `src/cli/cmd_show.cpp` — update node display/inspection

**IMPORTANT:** Changing edge lookup return types (from `vector<Edge const*>` to a span/view over arena data) affects ALL consumers. All files above must be updated in the SAME commit or the build will break.

This is the highest-impact change. The 4 edge index maps become:

```cpp
// In Graph struct:
Arena32 edge_arena;           // shared arena for all edge lists
NodeIdMap64 edges_to_index;   // NodeId → ArenaSlice (indices into edges vector)
NodeIdMap64 edges_from_index;
NodeIdMap64 order_only_to;    // NodeId → ArenaSlice (NodeId lists)
NodeIdMap64 order_only_deps;
```

- [ ] **Step 1: Replace edge index maps in dag.hpp**

Change the 4 `std::unordered_map<NodeId, std::vector<...>>` members to `NodeIdMap64` + shared `Arena32`. Keep the `std::vector<Edge> edges` central storage (Arena is for the index, not the edges themselves).

- [ ] **Step 2: Write test for arena-backed edge indices**

```cpp
TEST_CASE("Graph edge indices use Arena32", "[dag]")
{
    // Build a small graph with known edges
    // Call build_edge_indices()
    // Verify edges_to(node) and edges_from(node) return correct data
    // Verify order_only lookups work
}
```

- [ ] **Step 3: Update build_edge_indices() in dag.cpp**

`build_edge_indices()` currently iterates `edges` and populates the maps. Rewrite to:
1. Count edges per node (first pass)
2. Resize NodeIdMap64 arrays for all 4 node types
3. Allocate Arena slices per node
4. Fill Arena slices (second pass)

- [ ] **Step 4: Update edge lookup functions and ALL consumers**

Functions like `edges_to(NodeId)`, `edges_from(NodeId)` change return type. Update every file listed above in the same pass.

- [ ] **Step 5: Build, test full suite**

Run: `make && make test`
Expected: All 349+ tests pass

- [ ] **Step 6: Commit**

```bash
git commit -m "Migrate graph edge indices to NodeIdMap64 + Arena32

Replace 4 unordered_map<NodeId, vector<...>> with NodeIdMap64
storing ArenaSlice references into a shared Arena32."
```

---

### Task 8: Migrate DirNameKey to per-directory SortedPairVec ✅

**Completed.** Replaced `unordered_map<DirNameKey, NodeId, DirNameKeyHash, DirNameKeyEqual>` with `std::vector<SortedPairVec> dir_children` indexed by parent directory. Removed 4 types (`DirNameKey`, `DirNameKeyView`, `DirNameKeyHash`, `DirNameKeyEqual`). Modified only `dag.hpp` and `dag.cpp` (no builder.cpp changes needed — public API unchanged).

**Deviation from plan:** Skipped the two-phase compact-into-Arena32 step. `find_by_dir_name()` is called interleaved with `add_file_node()` during construction, so there's no clean construction/read-only boundary. `SortedPairVec` is already a contiguous array with binary search — cache-friendly enough for 5-30 children per directory.
```

---

### Task 9: Migrate topo.cpp

**Files:**
- Modify: `src/graph/topo.cpp` — replace `unordered_map<NodeId, Color>` and `unordered_map<NodeId, NodeId>`

Replace with `NodeIdMap32` for color tracking and parent pointers. Initialize with `resize_files(graph.next_file_id)`, `resize_commands(node_id::index(graph.next_command_id))`, etc.

- [ ] **Step 1: Replace DfsState maps with NodeIdMap32**
- [ ] **Step 2: Build, test `[topo]` and full suite**
- [ ] **Step 3: Commit**

```bash
git commit -m "Migrate topo.cpp from unordered_map to NodeIdMap32"
```

---

## Chunk 3: Builder, Evaluator, Scheduler, CLI

### Task 10: Migrate CommandNode vectors to ArenaSlice

**Files:**
- Modify: `include/pup/graph/dag.hpp` — change `CommandNode::inputs`/`outputs` from `vector<NodeId>` to `ArenaSlice`
- Modify: `src/graph/dag.cpp` — update `expand_instruction()` and other functions reading inputs/outputs
- Modify: `src/graph/builder.cpp` — update code that populates inputs/outputs during graph building
- Modify: `src/exec/scheduler.cpp` — update job creation (reads inputs/outputs for BuildJob)
- Modify: `src/cli/cmd_show.cpp` — update node display
- Modify: `src/cli/cmd_build.cpp` — update upstream walk, implicit dep handling
- Modify: `include/pup/index/entry.hpp` — update CommandEntry serialization (reads inputs/outputs)
- Modify: `src/index/entry.cpp` — update CommandEntry round-trip code

This is a pervasive change. The inputs/outputs are read from the Arena via `graph.edge_arena.get(cmd.inputs)`.

- [ ] **Step 1: Write test for ArenaSlice-backed command operands**

```cpp
TEST_CASE("CommandNode operands via ArenaSlice", "[dag]")
{
    // Build a graph with known command inputs/outputs
    // Verify Arena-backed access returns correct NodeIds
}
```

- [ ] **Step 2: Modify CommandNode struct and ALL consumers atomically**
- [ ] **Step 3: Build, test full suite, commit**

---

### Task 11: Migrate VarDb to SortedPairVec

**Files:**
- Modify: `include/pup/parser/eval.hpp` — change VarDb internals, add `StringPool*` member
- Modify: `src/parser/eval.cpp` — update set/get/contains/append/names/clear

Replace `unordered_map<string, string, StringHash, equal_to<>>` with `SortedPairVec` storing `(StringId, StringId)` pairs. VarDb gets a `StringPool*` member.

The API remains `string_view` at the boundary. Internally:
- `set(name, value)`: intern both, `pair_vec.insert(name_id, value_id)`
- `get(name)`: `pool_->find(name)` → `pair_vec.find(name_id)` → `pool_->get(value_id)`
- `contains(name)`: `pool_->find(name)` → `pair_vec.find(name_id) != nullptr`

- [ ] **Step 1: Write test for interned VarDb**

```cpp
TEST_CASE("VarDb with StringPool interning", "[eval]")
{
    auto pool = pup::StringPool {};
    auto db = pup::parser::VarDb { &pool };

    SECTION("set and get")
    {
        db.set("CC", "gcc");
        REQUIRE(db.get("CC") == "gcc");
    }

    SECTION("overwrite")
    {
        db.set("CC", "gcc");
        db.set("CC", "clang");
        REQUIRE(db.get("CC") == "clang");
    }

    SECTION("contains")
    {
        db.set("FOO", "bar");
        REQUIRE(db.contains("FOO"));
        REQUIRE_FALSE(db.contains("BAR"));
    }

    SECTION("get missing returns empty")
    {
        REQUIRE(db.get("NOPE") == "");
    }
}
```

- [ ] **Step 2: Implement, build, test full suite, commit**

---

### Task 12: Migrate builder maps

**Files:**
- Modify: `include/pup/graph/builder.hpp` — change macro maps, group maps, config/env var tracking
- Modify: `src/graph/builder.cpp`
- Modify: `include/pup/parser/eval.hpp` — update `EvalContext::var_config_deps`/`var_env_deps` pointer types to match new container types in `BuilderState`

Replace:
- `unordered_map<string, BangMacroDef>` → intern macro names, use `SortedPairVec` or `IdArray32` (BangMacroDef needs decomposition into StringIds + ArenaSlices)
- `unordered_map<string, vector<NodeId>>` (groups) → page-table: `IdArray64[group_name_id] → ArenaSlice`
- `unordered_set<string>` (included_files) → `IdBitSet` on interned StringIds
- `set<string>` (exported_vars, used_config_vars, etc.) → `SortedIdVec`
- `unordered_map<GroupKey, NodeId>` → concatenate + intern key, use `IdArray32`
- `unordered_map<string, NodeId>` (config/env var nodes) → `SortedPairVec`
- `unordered_map<string, set<string>>` (var deps) → `IdArray64[name_id] → ArenaSlice` of dep StringIds

This is the largest task. Break into sub-commits per map type.

- [ ] **Steps: Modify per map, build, test, commit after each logical group**

---

### Task 13: Migrate scheduler and CLI

**Files:**
- Modify: `src/exec/scheduler.cpp` — replace `set<NodeId>` with `IdBitSet`, `unordered_map<string, string>` with `SortedPairVec`
- Modify: `src/cli/cmd_build.cpp` — replace `set<NodeId>`, `set<string>`, `unordered_map<string, NodeId>`
- Modify: `src/cli/cmd_clean.cpp`, `src/cli/cmd_show.cpp`, etc.

Mechanical migration: same patterns as builder.

- [ ] **Steps: Modify, build, test, commit**

---

### Task 14: Migrate remaining set<string> instances

**Files:**
- Modify: `include/pup/exec/scheduler.hpp` — `BuildJob::exported_vars` from `set<string>` to `SortedIdVec`
- Modify: `include/pup/graph/builder.hpp` — remaining `set<string>` members
- Modify: `include/pup/parser/eval.hpp` — `imported_vars` from `unordered_set<string>` to `IdBitSet` on interned IDs

- [ ] **Steps: Modify, build, test, commit**

---

### Task 15: Migrate PathCache ✅

**Completed.** Replaced `using PathCache = unordered_map<NodeId, string>` with `struct PathCache { NodeIdMap32 ids; StringPool pool; }`. PathCache owns its own StringPool because `get_full_path` takes `Graph const&` (can't intern into `Graph::strings`). Modified only `dag.hpp` and `dag.cpp` — no caller changes needed (all access through free functions). cmd_build.cpp did not need changes (false positive in original plan).

---

### Task 16: Final cleanup

**Files:**
- All production files in `src/` and `include/`

- [ ] **Step 1: Verify no STL container headers remain**

```bash
grep -rn '<unordered_map>\|<unordered_set>\|<set>' src/ include/
```

Expected: zero hits (except possibly `<set>` if `std::set<DeferredOrderOnlyEdge>` is kept)

- [ ] **Step 2: Remove string_hash.hpp if unused**

```bash
grep -rn 'string_hash.hpp' src/ include/
```

If only included by files that no longer use it, remove the header.

- [ ] **Step 3: Full test suite**

Run: `make && make test`
Expected: All tests pass

- [ ] **Step 4: Binary size check**

```bash
size build/putup
```

Expected: `.text` decreases from 927 KB

- [ ] **Step 5: Commit**

```bash
git commit -m "Remove <unordered_map>, <unordered_set>, <set> from production code

Custom containers migration complete. All maps/sets replaced with
IdArray, IdBitSet, Arena, SortedIdVec/PairVec, and page-table lookups.
The only hash table is StringPool's internal Robin Hood index."
```

---

## Execution Notes

**NodeId flag bits:** NodeIds encode type in high bits (bit 31=command, 30=condition, 29=phi). `node_id::index(id)` strips flags to get the per-type array index. `NodeIdMap32`/`NodeIdMap64` dispatch to the correct sub-array.

**NodeIdMap resize convention:** `resize_files(n)` takes raw NodeId (no flags), so pass `graph.next_file_id`. `resize_commands(n)`, `resize_conditions(n)`, `resize_phis(n)` take the stripped index, so pass `node_id::index(graph.next_command_id)`, etc. The `set(NodeId, value)` method always calls `node_id::index()` internally.

**StringPool threading:** StringPool must be accessible from all modules that intern strings. During graph construction, a single `StringPool*` is passed through `BuilderContext`. For VarDb, the pool reference is set at construction time.

**BangMacroDef decomposition:** This struct contains `Expression` members (AST nodes) that reference strings. Full interning of BangMacroDef requires the AST to use StringId instead of std::string. This may be deferred if the AST migration is too invasive — in that case, keep `unordered_map<string, BangMacroDef>` temporarily and note it as remaining tech debt.

**DeferredOrderOnlyEdge:** The `set<DeferredOrderOnlyEdge>` in builder.hpp is small, transient, and uses a composite comparator. Keep as `std::set` until a later pass — it's not worth a custom container for <100 elements with a two-field comparison.

**`set<pair<NodeId, NodeId>>`** in cmd_build.cpp: Keep as `std::set` — small, transient, composite key. Not worth a custom container.

**PathCache:** `using PathCache = unordered_map<NodeId, string>` in dag.hpp. Migrate to `NodeIdMap32` mapping `NodeId → StringId` (intern the path string). Must be addressed before Task 15 cleanup.

**Dependency DAG:**
```
Task 1 (IdBitSet)          ─┐
Task 2 (IdArray) ───────────┤─→ Task 6 (NodeIdMap) ─→ Task 7 (edge indices) ─→ Task 10 (CommandNode ArenaSlice)
Task 3 (Arena) ─────────────┤                      ─→ Task 8 (DirNameKey)
                            │                      ─→ Task 9 (topo.cpp)
Task 4 (SortedIdVec) ───────┤─→ Task 11 (VarDb)
Task 5 (StringPool RH) ─────┤─→ Task 12 (builder maps)
                            │─→ Task 13 (scheduler + CLI)
                            │─→ Task 14 (remaining sets)
                            └─→ Task 15 (cleanup) [depends on ALL]
```

Tasks 11-14 do NOT depend on Tasks 6-9. They depend on primitives (1-5) directly.
