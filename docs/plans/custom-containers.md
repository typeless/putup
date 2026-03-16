# Custom Containers — Design Spec

## Goal

Replace `std::unordered_map`, `std::unordered_set`, `std::set`, and eventually `std::vector` with tailored primitives. Part of the `-nostdlib++` trajectory.

## Core Insight

If all strings are interned, every key and value in the codebase reduces to a fixed-width integer. The container zoo collapses to 3 primitives + a string pool.

| Current Type | After Interning | Width |
|---|---|---|
| `std::string` (as key or value) | `StringId` | 32-bit |
| `NodeId` | `NodeId` | 32-bit |
| `DirNameKey` (parent_dir, name) | two-level lookup | — |
| `Color` (enum) | `uint8_t` (pad to 32) | 32-bit |
| `vector<NodeId>` (edges, inputs) | arena slice `{offset, len}` | 64-bit |
| `set<string>` | sorted `StringId[]` | 32-bit × N |

## Primitives

| Primitive | Mechanism | Replaces |
|---|---|---|
| **IdArray** | flat array indexed by dense ID | all `unordered_map<NodeId, V>` |
| **IdBitSet** | bitset indexed by dense ID | all `set<NodeId>`, `unordered_set<NodeId>` |
| **Arena** | append-only pool + `{offset, len}` slices | `vector<NodeId>`, edge lists, operands |
| **SortedIdVec** | sorted array of integer IDs | small `set<string>`, `set<StringId>` |
| **StringPool** | the one string↔integer bridge | string interning (already exists) |

No general-purpose hash table outside of `StringPool`. The `DirNameKey` reverse lookup uses a page-table-style two-level dense structure instead.

## Design Principles

**Tailored, not templated.** No C++ templates. Each primitive has concrete implementations per element width (32 or 64 bit).

**Raw memory, not std::vector.** Primitives own memory via `malloc`/`realloc`/`free`. This avoids circular dependency when `std::vector` is later replaced.

**Two-phase lifecycle.** `reserve(n)` for upfront allocation, `compact()` for right-sizing after population. Fits putup's build-once-then-query pattern.

**Pervasive interning.** All string storage goes through `StringPool`. Variables, paths, commands, display strings — everything becomes a `StringId`. External strings enter the system through `intern()` at the boundary and leave through `get()` at output.

**Dense over sparse.** Prefer flat arrays (O(1), zero overhead) over hash tables. The only hash table in the system is `StringPool`'s internal index — and even that is a specialized Robin Hood table for `string_view → StringId`, not a general-purpose container.

## Primitive Designs

### StringPool (existing — to be expanded)

Already exists (`core/string_pool.hpp`). Currently used only for `Graph::strings`. Must become the universal interning layer.

**Expansion:** All string-owning maps (`VarDb`, macro names, group names, env var names, file paths) migrate to `StringId` references into a shared pool.

**Internal migration:** Replace `unordered_map<string_view, StringId>` index with a Robin Hood table. This is the ONE hash table in the system — specialized for `string_view` keys with `fnv1a` hash. Implemented directly in `string_pool.cpp`.

**File:** `core/string_pool.{hpp,cpp}` (modified)

### IdArray

Flat array indexed by dense integer ID. O(1) read/write.

```
capacity: N
data:     [slot_0, slot_1, slot_2, ..., slot_N-1]
present:  IdBitSet tracking which slots are occupied
```

Each slot holds a fixed-width value. Unoccupied slots are zero-initialized. The parallel `IdBitSet` distinguishes "not present" from "present with value 0".

Two concrete types:
- `IdArray32` — 32-bit values (NodeId, StringId, Color, counts)
- `IdArray64` — 64-bit values (arena slices `{offset:32, len:32}`)

**Operations:** `set(id, value)`, `get(id) -> value`, `contains(id) -> bool`, `resize(max_id)`, `clear()`, `for_each(callback)`.

**Implementation:** `malloc`/`realloc`. No constructors/destructors — values are plain integers.

**File:** `core/id_array.{hpp,cpp}`

### IdBitSet

Bitset with capacity equal to max ID. O(1) insert/test, cache-friendly iteration.

```
word[0] = bits 0..63
word[1] = bits 64..127
...
```

**Operations:** `insert(id)`, `contains(id)`, `remove(id)`, `clear()`, `count()`, `for_each(callback)`, `resize(max_id)`.

**Implementation:** `malloc`/`realloc` for `uint64_t` word array. Concrete type.

**File:** `core/id_bitset.{hpp,cpp}`

### Arena

Append-only pool of integer values. Variable-length lists per entity are stored as `{offset, length}` slice references.

```
data: [a₀, a₁, a₂, b₀, b₁, c₀, c₁, c₂, c₃, ...]
       ↑ list A     ↑ list B  ↑ list C
```

```cpp
struct ArenaSlice {
    uint32_t offset;
    uint32_t length;
};
```

An `IdArray64` stores `ArenaSlice` values per entity — e.g., `edges_to[node_id] = {offset=5, length=3}`.

**Operations:**
- `append(values...) -> ArenaSlice` — add a list, return its slice
- `get(slice) -> {ptr, len}` — access a stored list
- `reserve(total_elements)` — pre-allocate
- `compact()` — shrink to fit

**Implementation:** Single `malloc`/`realloc` buffer of `uint32_t`. Append-only during build, immutable after compact.

**File:** `core/arena.{hpp,cpp}`

### SortedIdVec

Sorted array of 32-bit IDs with hand-written binary search. For small sparse sets (<100 elements).

**Operations:** `insert(id)`, `contains(id)`, `remove(id)`, `clear()`, `size()`, range-for.

**Implementation:** `malloc`/`realloc`. Binary search for lookup. `memmove` to maintain sorted order on insert.

**Replaces:** `set<StringId>` (exported_vars), `set<string>` (after interning), small `unordered_set<string>`.

**File:** `core/sorted_id_vec.{hpp,cpp}`

## DirNameKey: Page-Table Lookup

The `dir_name_index` maps `(parent_dir, name) → NodeId`. This is a reverse lookup over a product of two dense ID spaces. Instead of a hash table, use a page-table-style two-level structure:

```
Level 1:  IdArray64[parent_dir] → ArenaSlice into Level 2
Level 2:  Arena of sorted (StringId, NodeId) pairs per directory

Lookup:   parent_dir → ArenaSlice → binary search on StringId → NodeId
```

**Why this works:**
- Each directory has ~5-50 children. Binary search on 50 entries is ~6 comparisons.
- Tupfile processing is directory-local — repeated lookups hit the same L1 entry, keeping the L2 slice hot in cache (same principle as TLB locality).
- No hash table needed. Composes from existing primitives (IdArray + Arena).
- Memory-proportional to actual entries. Zero waste.

**Incremental insert:** During graph construction, nodes are added one at a time. Maintaining sorted order within a directory's Arena slice costs O(K) per insert (memmove within the slice, K = children per directory ≈ 20). Total cost ~200K comparisons for 10K nodes — negligible for a build system about to spawn compiler processes.

**Alternative (build-then-sort):** Collect all `(parent_dir, name, NodeId)` tuples during construction, sort by `parent_dir` then `name`, and build the two-level structure in one pass. This avoids per-insert memmove but requires that lookups aren't needed during construction. If the graph builder can be restructured to separate node creation from node lookup, this is cleaner.

**File:** Integrated into `graph/dag.{hpp,cpp}` (replaces `dir_name_index` map)

## GroupKey Migration

`unordered_map<GroupKey, NodeId>` where `GroupKey = (directory, name)`. Group names are identifiers that never contain `/`.

After interning, `directory` becomes a `StringId` and `name` becomes a `StringId`. Use the same page-table approach as DirNameKey:

```
Level 1:  IdArray64[dir_string_id] → ArenaSlice into Level 2
Level 2:  Arena of sorted (StringId, NodeId) pairs per directory
```

Or simpler: concatenate `directory + "/" + name` (safe because group names are identifiers with no slashes), intern the result, and use `IdArray32[concatenated_string_id] → NodeId`.

## VarDb Migration

`VarDb` currently stores `unordered_map<string, string>`. After interning:

```cpp
class VarDb {
    IdArray32 vars_;    // StringId(name) → StringId(value)
    StringPool* pool_;
public:
    auto set(std::string_view name, std::string_view value) -> void;
    auto get(std::string_view name) const -> std::string_view;
    auto contains(std::string_view name) const -> bool;
};
```

Wait — `IdArray32` requires dense keys. StringIds are dense (sequential from the pool). But VarDb typically has ~100-1000 variables while the StringPool may have ~10,000 interned strings. The IdArray would be 10,000 slots with 100 occupied — 99% waste.

**Better approach:** VarDb uses a `SortedIdVec`-style structure: sorted array of `(StringId, StringId)` pairs. Binary search on the name StringId. For ~100-1000 variables, binary search is ~10 comparisons — fast enough. Insert maintains sorted order via memmove.

```cpp
class VarDb {
    uint32_t* data_;      // interleaved [name₀, value₀, name₁, value₁, ...]
    size_t count_;
    size_t capacity_;
    StringPool* pool_;
};
```

Or introduce a **SortedPairVec** (sorted array of `(uint32_t key, uint32_t value)` pairs).

## Graph Node Migration

`FileNode` and `CommandNode` currently store `std::string` members. After interning:

```cpp
struct FileNode {
    NodeId id;
    StringId name;        // was: std::string name
    StringId dir_name;    // was: std::string dir_name
    NodeType type;
    // ...
};

struct CommandNode {
    NodeId id;
    StringId command;     // was: std::string command
    StringId display;     // was: std::string display
    ArenaSlice inputs;    // was: std::vector<NodeId> inputs
    ArenaSlice outputs;   // was: std::vector<NodeId> outputs
    // ...
};
```

Edge lists become `ArenaSlice` references into a shared `Arena32`. This replaces both per-node `vector<NodeId>` allocations and the `unordered_map<NodeId, vector<NodeId>>` edge index.

## Remaining `std::set` Instances

| Instance | Location | Migration |
|---|---|---|
| `set<string>` (exported_vars, config_vars, etc.) | builder.hpp, eval.hpp | `SortedIdVec` after interning |
| `set<StringId>` (CommandNode::exported_vars) | dag.hpp | `SortedIdVec` |
| `set<DeferredOrderOnlyEdge>` | builder.hpp | Keep as-is (transient, <100 elements, composite comparator) |
| `set<pair<NodeId, NodeId>>` | cmd_build.cpp | `IdBitSet` on packed `(a<<32\|b)`, or keep as-is if count is small |

## Remaining `std::unordered_*` Instances

| Instance | Location | Migration |
|---|---|---|
| `unordered_map<string, string>` (VarDb) | eval.hpp | SortedPairVec (StringId→StringId) |
| `unordered_map<string, BangMacroDef>` | builder.hpp | Intern key → SortedPairVec or IdArray (BangMacroDef becomes a struct of StringIds + ArenaSlices) |
| `unordered_map<string, vector<NodeId>>` | builder.hpp (groups) | Page-table: IdArray64[group_name_id] → ArenaSlice |
| `unordered_map<GroupKey, NodeId>` | builder.hpp | Page-table or concatenated StringId |
| `unordered_map<string, NodeId>` | builder.hpp (config/env var nodes) | IdArray32[interned_name] → NodeId |
| `unordered_map<string, set<string>>` | builder.hpp (var deps) | IdArray64[name_id] → ArenaSlice of dep StringIds |
| `unordered_set<string>` | builder.hpp, eval.hpp | SortedIdVec or IdBitSet |
| `unordered_map<string_view, StringId>` | string_pool.hpp | Internal Robin Hood (the one hash table) |
| `unordered_map<string, string>` (env cache) | scheduler.cpp | SortedPairVec |
| `unordered_map<string, NodeId>` | cmd_build.cpp | IdArray32 or SortedPairVec |
| `unordered_set<size_t>` | scheduler.cpp | IdBitSet (job indices are dense) |

## Migration Order

### Step 1: Primitives
Build and test all primitives in isolation. No production code changes yet.
- `IdArray32`, `IdArray64`
- `IdBitSet`
- `Arena32`
- `SortedIdVec`
- `SortedPairVec` (sorted `(uint32_t, uint32_t)` pairs)
- `StringPool` internal Robin Hood index

### Step 2: Graph internals
Migrate `dag.hpp` node types to `StringId` + `ArenaSlice`. Replace edge index maps with `IdArray64`. Replace `dir_name_index` with page-table two-level lookup. Replace traversal sets with `IdBitSet`. This is the highest-impact change.

### Step 3: Builder + evaluator
Migrate `VarDb` to `SortedPairVec`. Migrate macro maps, group maps, config/env var tracking. Expand `StringPool` to cover all string storage in builder context.

### Step 4: Scheduler + CLI
Migrate remaining maps in scheduler and CLI commands. Replace remaining `set<string>` with `SortedIdVec`.

### Step 5: Cleanup
Remove `<unordered_map>`, `<unordered_set>`, `<set>` from all production includes. Remove `core/string_hash.hpp`. Verify zero STL container headers remain.

## Files

| File | Content |
|---|---|
| `include/pup/core/id_array.hpp` | IdArray32, IdArray64 |
| `src/core/id_array.cpp` | Implementation |
| `include/pup/core/id_bitset.hpp` | IdBitSet |
| `src/core/id_bitset.cpp` | Implementation |
| `include/pup/core/arena.hpp` | Arena32, ArenaSlice |
| `src/core/arena.cpp` | Implementation |
| `include/pup/core/sorted_id_vec.hpp` | SortedIdVec, SortedPairVec |
| `src/core/sorted_id_vec.cpp` | Implementation |
| `core/string_pool.{hpp,cpp}` | Modified — expanded + internal Robin Hood index |

## Testing

Each primitive gets its own test file:
- `test_id_array.cpp` — dense access, out-of-range, parallel bitset tracking
- `test_id_bitset.cpp` — insert/contains/remove, clear, count, for_each, resize
- `test_arena.cpp` — append, get slice, reserve, compact
- `test_sorted_id_vec.cpp` — ordering, duplicate insert, contains, remove, binary search
- `test_string_pool.cpp` — expanded: Robin Hood index, large-scale interning

## Success Criteria

- Zero `<unordered_map>`, `<unordered_set>`, `<set>` in production includes
- All tests pass (current 349+ plus new primitive tests)
- Binary `.text` decreases
- No performance regression (self-hosting build time within 10%)
- All string data flows through `StringPool`
- No general-purpose hash table outside of `StringPool`
- `DirNameKey` lookup uses page-table structure, not hash table
