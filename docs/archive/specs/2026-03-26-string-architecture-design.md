# String Architecture: StringId + Buf + HeapBuf

## Goal

Replace `pup::String` (24-byte owned SSO string) with a three-tier string model:
- **StringId** — 4-byte interned handle for storage
- **string_view** — for reading
- **Buf / HeapBuf** — for building

All constant strings are interned into a global `StringPool`. No owned string type is stored as a struct member. No SSO needed.

## Motivation

`pup::String` is an awkward middle ground — it owns bytes like `std::string` but doesn't intern like `StringId`. Short strings (variable names, basenames) should be `StringId` (4 bytes). Long strings (expanded commands, paths) are built in a buffer then interned. The SSO in `pup::String` is wasted complexity.

## Types

### StringId (existing — no changes)

```cpp
enum class StringId : uint32_t { Empty = 0 };
```

The default storage type. Every struct member that holds a string becomes `StringId`. Resolved via `global_pool().get(id) -> string_view`.

### Buf — Stack-first scratch buffer

```cpp
class Buf {
public:
    Buf() = default;
    ~Buf();

    Buf(Buf const&) = delete;
    auto operator=(Buf const&) -> Buf& = delete;
    Buf(Buf&&) = delete;
    auto operator=(Buf&&) -> Buf& = delete;

    auto append(std::string_view sv) -> void;
    auto append(char c) -> void;
    auto operator+=(std::string_view sv) -> Buf&;
    auto operator+=(char c) -> Buf&;

    template<typename... Args>
    auto fmt(std::string_view pattern, Args const&... args) -> void;

    auto reserve(std::size_t n) -> void;
    auto clear() -> void;

    [[nodiscard]] auto data() const -> char const*;
    [[nodiscard]] auto data() -> char*;
    [[nodiscard]] auto size() const -> std::size_t;
    [[nodiscard]] auto empty() const -> bool;
    [[nodiscard]] auto c_str() const -> char const*;
    [[nodiscard]] auto view() const -> std::string_view;

    [[nodiscard]] auto intern(StringPool& pool) const -> StringId;

private:
    static constexpr std::size_t INLINE_CAP = 256;
    char buf_[INLINE_CAP];
    char* data_ = buf_;
    std::uint32_t size_ = 0;
    std::uint32_t capacity_ = INLINE_CAP;
};
```

- 272 bytes. Stack-local only — non-copyable, non-movable.
- Starts inline. Overflows to heap (malloc) if content exceeds 256 bytes.
- Destructor frees heap if `data_ != buf_`.
- `fmt()` is a method — the buffer is the format destination.
- `intern()` interns the content into a pool and returns the StringId.
- `view()` returns a string_view into the buffer (valid while Buf is alive).
- The default choice for transient string building.

### HeapBuf — Movable heap buffer

```cpp
class HeapBuf {
public:
    HeapBuf() = default;
    ~HeapBuf();

    HeapBuf(HeapBuf const&) = delete;
    auto operator=(HeapBuf const&) -> HeapBuf& = delete;
    HeapBuf(HeapBuf&&) noexcept;
    auto operator=(HeapBuf&&) noexcept -> HeapBuf&;

    auto append(std::string_view sv) -> void;
    auto append(char c) -> void;
    auto operator+=(std::string_view sv) -> HeapBuf&;
    auto operator+=(char c) -> HeapBuf&;

    template<typename... Args>
    auto fmt(std::string_view pattern, Args const&... args) -> void;

    auto reserve(std::size_t n) -> void;
    auto resize(std::size_t n) -> void;
    auto clear() -> void;

    [[nodiscard]] auto data() const -> char const*;
    [[nodiscard]] auto data() -> char*;
    [[nodiscard]] auto size() const -> std::size_t;
    [[nodiscard]] auto empty() const -> bool;
    [[nodiscard]] auto c_str() const -> char const*;
    [[nodiscard]] auto view() const -> std::string_view;

    [[nodiscard]] auto intern(StringPool& pool) const -> StringId;

private:
    char* data_ = nullptr;
    std::uint32_t size_ = 0;
    std::uint32_t capacity_ = 0;
};
```

- 16 bytes. Heap-only. Movable (for returning from functions).
- Used when the built string needs to outlive the current scope (return from function, intern later).
- Same append/fmt API as Buf.

### Global StringPool

```cpp
auto global_pool() -> StringPool&;
```

A process-wide singleton. All interned strings live here. Outlives everything. Replaces `Graph::strings` as the primary pool.

Implementation: function-local static.

Thread safety: single-threaded access during parsing (all interning happens on main thread). Worker threads only run commands and read results.

## Usage Patterns

### Storing a string (struct member)

```cpp
// Before:
struct BuilderOptions {
    String source_root;
};

// After:
struct BuilderOptions {
    StringId source_root;
};
```

### Reading a string

```cpp
// Before:
printf("%s\n", opts.source_root.c_str());

// After:
auto sv = global_pool().get(opts.source_root);
printf("%.*s\n", (int)sv.size(), sv.data());
```

### Building a transient string (error message, display output)

```cpp
// Before:
auto msg = String { "path not found: " } + path;
fprintf(stderr, "%s\n", msg.c_str());

// After:
auto buf = Buf {};
buf.fmt("path not found: {}", path);
fprintf(stderr, "%s\n", buf.c_str());
```

### Building a string to store

```cpp
// Before:
auto result = path::join(parent, name);
entry.path = result;

// After:
auto buf = HeapBuf {};
buf.append(parent);
buf += '/';
buf.append(name);
entry.path = buf.intern(global_pool());
```

### PatternFlags (string_view into pool)

```cpp
// Before:
struct PatternFlags {
    String input;       // owned copy
    String input_base;  // owned copy
};

// After:
struct PatternFlags {
    std::string_view input;       // view into pool
    std::string_view input_base;  // substring of input
};
```

## Migration Scope

### Phase 1: Primitives
- Create `Buf` (`include/pup/core/buf.hpp`, `src/core/buf.cpp`)
- Create `HeapBuf` (`include/pup/core/heap_buf.hpp`, `src/core/heap_buf.cpp`)
- Create `global_pool()` (`include/pup/core/global_pool.hpp`, `src/core/global_pool.cpp`)
- Move `fmt()` logic into Buf/HeapBuf methods
- Write tests for all three

### Phase 2: Struct member migration
- Change `pup::String` members to `StringId` in headers, inside-out:
  - graph layer (builder.hpp, rule_pattern.hpp, dag.hpp)
  - index layer (entry.hpp)
  - parser layer (ast.hpp, eval.hpp, glob.hpp, ignore.hpp, var_tracking.hpp, parser.hpp, depfile.hpp)
  - exec layer (scheduler.hpp, runner.hpp, progress_display.hpp)
  - cli layer (options.hpp, context.hpp, target.hpp, config_commands.hpp)
  - core layer (result.hpp, layout.hpp)
- Fix cascading .cpp errors at each step
- `Graph::strings` becomes an alias or wrapper around `global_pool()`

### Phase 3: Remove pup::String
- Delete `include/pup/core/string.hpp` and `src/core/string.cpp`
- Remove `pup::String` from all remaining .cpp local variables
- `pup::fmt()` free function replaced by `Buf::fmt()` / `HeapBuf::fmt()`
- `path::join`, `path::normalize`, `path::relative` return `StringId` (intern internally)
  - Data-driven: pool pollution from temporary path strings is <3% overhead in real builds (29 temporary calls out of 4,643 pool entries in GCC cross-compile build)

### Design principles

- **Return value = immutable** — functions return `StringId` (interned) or `string_view` (borrowed)
- **Output parameter = mutable** — functions take `Buf&` / `HeapBuf&` when the caller needs to keep building
- **No owned mutable string passed around** — that was `pup::String`'s role, and it's eliminated

## What stays unchanged

- `StringPool` internals (Robin Hood hash, dedup, `intern()`, `get()`, `find()`)
- `StringId` type and helpers (`to_underlying`, `make_string_id`, `is_empty`)
- `VarDb` (already uses `SortedPairVec<StringId, StringId>`)
- `std::string_view` usage throughout (already the standard read type)
- `SortedPairVec`, `SortedIdVec`, `NodeIdMap32` (operate on uint32_t / StringId)

## Files modified

| File | Change |
|------|--------|
| `include/pup/core/buf.hpp` | New: Buf type |
| `src/core/buf.cpp` | New: Buf implementation |
| `include/pup/core/heap_buf.hpp` | New: HeapBuf type |
| `src/core/heap_buf.cpp` | New: HeapBuf implementation |
| `include/pup/core/global_pool.hpp` | New: global_pool() accessor |
| `src/core/global_pool.cpp` | New: singleton implementation |
| `include/pup/core/string.hpp` | Eventually deleted |
| `src/core/string.cpp` | Eventually deleted |
| `include/pup/core/fmt.hpp` | Eventually deleted (fmt moves into Buf/HeapBuf) |
| `src/core/fmt.cpp` | Eventually deleted |
| ~30 production headers | `String` members → `StringId` |
| ~20 production .cpp files | Callers use `global_pool().get(id)` and `Buf`/`HeapBuf` |

## Success criteria

- Zero `pup::String` in production code
- All struct members use `StringId` (4 bytes) for string storage
- `Buf` used for transient formatting (stack-local, no heap for < 256 bytes)
- `HeapBuf` used for building strings that will be interned or returned
- All tests pass
- Binary size same or smaller than current (563 KB .text)
