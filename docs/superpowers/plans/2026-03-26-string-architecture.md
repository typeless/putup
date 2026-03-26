# String Architecture Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace `pup::String` with a three-tier string model: `StringId` for storage, `string_view` for reading, `Buf`/`HeapBuf` for building.

**Architecture:** All struct members become 4-byte `StringId` handles resolved via a global `StringPool`. String building uses `Buf` (stack-first 272 bytes, non-movable) for transient formatting and `HeapBuf` (heap-only 16 bytes, movable) for strings returned from functions. `pup::String` is eliminated entirely.

**Tech Stack:** C++20, malloc/realloc/free, existing StringPool/StringId, Catch2 tests, putup build system.

**Spec:** `docs/superpowers/specs/2026-03-26-string-architecture-design.md`

**Build/test commands:**
```bash
make                                      # Build (incremental)
make clean && make                        # Clean build (needed after header changes)
./build/test/unit/putup_test '[buf]'      # Buf tests
./build/test/unit/putup_test '[heap_buf]' # HeapBuf tests
./build/test/unit/putup_test              # All tests
make format                               # clang-format
```

---

## File Map

| File | Role | Change |
|------|------|--------|
| `include/pup/core/buf.hpp` | New: Buf (stack-first buffer) | Create |
| `src/core/buf.cpp` | New: Buf implementation | Create |
| `include/pup/core/heap_buf.hpp` | New: HeapBuf (heap buffer) | Create |
| `src/core/heap_buf.cpp` | New: HeapBuf implementation | Create |
| `include/pup/core/format_to.hpp` | New: format_impl that writes to any buffer | Create |
| `include/pup/core/global_pool.hpp` | New: global_pool() accessor | Create |
| `src/core/global_pool.cpp` | New: singleton implementation | Create |
| `include/pup/core/string.hpp` | Eventually deleted (Phase 3) | Delete |
| `src/core/string.cpp` | Eventually deleted (Phase 3) | Delete |
| `include/pup/core/fmt.hpp` | Eventually deleted (Phase 3) | Delete |
| `src/core/fmt.cpp` | Eventually deleted (Phase 3) | Delete |
| ~30 production headers | `String` members → `StringId` | Modify |
| ~20 production .cpp files | Callers use `global_pool().get()` + `Buf`/`HeapBuf` | Modify |
| `test/unit/test_buf.cpp` | New: Buf tests | Create |
| `test/unit/test_heap_buf.cpp` | New: HeapBuf tests | Create |

---

## Phase 1: Primitives

### Task 1: HeapBuf

The simpler of the two buffers. No inline storage, just `char* + size + capacity` with malloc/realloc.

**Files:**
- Create: `include/pup/core/heap_buf.hpp`
- Create: `src/core/heap_buf.cpp`
- Create: `test/unit/test_heap_buf.cpp`
- Modify: `Tupfile` — add `src/core/heap_buf.cpp`
- Modify: `test/unit/Tupfile` — add `test_heap_buf.cpp`

- [ ] **Step 1: Write test**

```cpp
// test/unit/test_heap_buf.cpp
#include "catch_amalgamated.hpp"
#include "pup/core/heap_buf.hpp"
#include "pup/core/string_pool.hpp"

using pup::HeapBuf;

TEST_CASE("HeapBuf basic operations", "[heap_buf]")
{
    auto buf = HeapBuf {};

    SECTION("default is empty")
    {
        REQUIRE(buf.empty());
        REQUIRE(buf.size() == 0);
    }

    SECTION("append and view")
    {
        buf.append("hello");
        buf += ' ';
        buf += "world";
        REQUIRE(buf.view() == "hello world");
        REQUIRE(buf.size() == 11);
    }

    SECTION("c_str is null-terminated")
    {
        buf.append("test");
        REQUIRE(buf.c_str()[4] == '\0');
    }

    SECTION("clear resets")
    {
        buf.append("data");
        buf.clear();
        REQUIRE(buf.empty());
    }

    SECTION("reserve pre-allocates")
    {
        buf.reserve(1000);
        buf.append("after reserve");
        REQUIRE(buf.view() == "after reserve");
    }

    SECTION("resize and mutable data")
    {
        buf.resize(5);
        REQUIRE(buf.size() == 5);
        auto* p = buf.data();
        p[0] = 'h'; p[1] = 'i'; p[2] = '\0';
    }

    SECTION("large append triggers growth")
    {
        for (int i = 0; i < 1000; ++i) {
            buf += 'x';
        }
        REQUIRE(buf.size() == 1000);
    }
}

TEST_CASE("HeapBuf move", "[heap_buf]")
{
    auto a = HeapBuf {};
    a.append("movable");
    auto b = HeapBuf { std::move(a) };
    REQUIRE(b.view() == "movable");
    REQUIRE(a.empty());
}

TEST_CASE("HeapBuf intern", "[heap_buf]")
{
    auto pool = pup::StringPool {};
    auto buf = HeapBuf {};
    buf.append("interned");
    auto id = buf.intern(pool);
    REQUIRE(pool.get(id) == "interned");
}
```

- [ ] **Step 2: Add to Tupfiles**

In `Tupfile` after `srcs-y += src/core/fmt.cpp`:
```
srcs-y += src/core/heap_buf.cpp
```

In `test/unit/Tupfile` after `test-srcs-y += test_hash.cpp`:
```
test-srcs-y += test_heap_buf.cpp
```

- [ ] **Step 3: Implement HeapBuf**

`include/pup/core/heap_buf.hpp`:
```cpp
// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include "pup/core/string_id.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace pup {

class StringPool;

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

    auto reserve(std::size_t n) -> void;
    auto resize(std::size_t n) -> void;
    auto clear() -> void;

    [[nodiscard]] auto data() const -> char const* { return data_; }
    [[nodiscard]] auto data() -> char* { return data_; }
    [[nodiscard]] auto size() const -> std::size_t { return size_; }
    [[nodiscard]] auto empty() const -> bool { return size_ == 0; }
    [[nodiscard]] auto c_str() const -> char const* { return data_ ? data_ : ""; }
    [[nodiscard]] auto view() const -> std::string_view { return { data_, size_ }; }

    [[nodiscard]] auto intern(StringPool& pool) const -> StringId;

private:
    char* data_ = nullptr;
    std::uint32_t size_ = 0;
    std::uint32_t capacity_ = 0;

    auto grow(std::size_t needed) -> void;
};

} // namespace pup
```

`src/core/heap_buf.cpp`:
```cpp
// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/core/heap_buf.hpp"
#include "pup/core/string_pool.hpp"

#include <cstdlib>
#include <cstring>

namespace pup {

HeapBuf::~HeapBuf()
{
    std::free(data_);
}

HeapBuf::HeapBuf(HeapBuf&& other) noexcept
    : data_(other.data_)
    , size_(other.size_)
    , capacity_(other.capacity_)
{
    other.data_ = nullptr;
    other.size_ = 0;
    other.capacity_ = 0;
}

auto HeapBuf::operator=(HeapBuf&& other) noexcept -> HeapBuf&
{
    if (this != &other) {
        std::free(data_);
        data_ = other.data_;
        size_ = other.size_;
        capacity_ = other.capacity_;
        other.data_ = nullptr;
        other.size_ = 0;
        other.capacity_ = 0;
    }
    return *this;
}

auto HeapBuf::grow(std::size_t needed) -> void
{
    if (needed <= capacity_) {
        return;
    }
    auto new_cap = static_cast<std::size_t>(capacity_) + capacity_ / 2 + 16;
    if (new_cap < needed) {
        new_cap = needed;
    }
    auto* p = static_cast<char*>(std::realloc(data_, new_cap));
    if (!p) {
        std::abort();
    }
    data_ = p;
    capacity_ = static_cast<std::uint32_t>(new_cap);
}

auto HeapBuf::append(std::string_view sv) -> void
{
    if (sv.empty()) {
        return;
    }
    auto new_size = size_ + sv.size();
    grow(new_size + 1);
    std::memcpy(data_ + size_, sv.data(), sv.size());
    size_ = static_cast<std::uint32_t>(new_size);
    data_[size_] = '\0';
}

auto HeapBuf::append(char c) -> void
{
    grow(size_ + 2);
    data_[size_++] = c;
    data_[size_] = '\0';
}

auto HeapBuf::operator+=(std::string_view sv) -> HeapBuf&
{
    append(sv);
    return *this;
}

auto HeapBuf::operator+=(char c) -> HeapBuf&
{
    append(c);
    return *this;
}

auto HeapBuf::reserve(std::size_t n) -> void
{
    grow(n + 1);
}

auto HeapBuf::resize(std::size_t n) -> void
{
    grow(n + 1);
    if (n > size_) {
        std::memset(data_ + size_, 0, n - size_);
    }
    size_ = static_cast<std::uint32_t>(n);
    data_[size_] = '\0';
}

auto HeapBuf::clear() -> void
{
    size_ = 0;
    if (data_) {
        data_[0] = '\0';
    }
}

auto HeapBuf::intern(StringPool& pool) const -> StringId
{
    return pool.intern(view());
}

} // namespace pup
```

- [ ] **Step 4: Build and run tests**

Run: `make && ./build/test/unit/putup_test '[heap_buf]'`
Expected: All tests pass.

- [ ] **Step 5: Run full suite, commit**

Run: `./build/test/unit/putup_test`
Expected: All 453+ tests pass.

```bash
git add include/pup/core/heap_buf.hpp src/core/heap_buf.cpp \
        test/unit/test_heap_buf.cpp Tupfile test/unit/Tupfile
git commit -m "Add HeapBuf — 16-byte movable heap string buffer

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>"
```

---

### Task 2: Buf (stack-first buffer)

Same API as HeapBuf but with 256-byte inline storage. Non-copyable, non-movable.

**Files:**
- Create: `include/pup/core/buf.hpp`
- Create: `src/core/buf.cpp`
- Create: `test/unit/test_buf.cpp`
- Modify: `Tupfile` — add `src/core/buf.cpp`
- Modify: `test/unit/Tupfile` — add `test_buf.cpp`

- [ ] **Step 1: Write test**

```cpp
// test/unit/test_buf.cpp
#include "catch_amalgamated.hpp"
#include "pup/core/buf.hpp"
#include "pup/core/string_pool.hpp"

using pup::Buf;

TEST_CASE("Buf basic operations", "[buf]")
{
    auto buf = Buf {};

    SECTION("default is empty")
    {
        REQUIRE(buf.empty());
        REQUIRE(buf.size() == 0);
    }

    SECTION("append stays inline")
    {
        buf.append("hello");
        REQUIRE(buf.view() == "hello");
    }

    SECTION("c_str is null-terminated")
    {
        buf.append("test");
        REQUIRE(buf.c_str()[4] == '\0');
    }

    SECTION("operator+=")
    {
        buf += "a";
        buf += 'b';
        buf += "c";
        REQUIRE(buf.view() == "abc");
    }

    SECTION("overflow to heap")
    {
        for (int i = 0; i < 300; ++i) {
            buf += 'x';
        }
        REQUIRE(buf.size() == 300);
        REQUIRE(buf.view().substr(0, 3) == "xxx");
    }

    SECTION("clear resets")
    {
        buf.append("data");
        buf.clear();
        REQUIRE(buf.empty());
    }
}

TEST_CASE("Buf intern", "[buf]")
{
    auto pool = pup::StringPool {};
    auto buf = Buf {};
    buf.append("interned");
    auto id = buf.intern(pool);
    REQUIRE(pool.get(id) == "interned");
}
```

- [ ] **Step 2: Add to Tupfiles and implement**

`include/pup/core/buf.hpp`:
```cpp
// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include "pup/core/string_id.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace pup {

class StringPool;

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

    auto reserve(std::size_t n) -> void;
    auto clear() -> void;

    [[nodiscard]] auto data() const -> char const* { return data_; }
    [[nodiscard]] auto data() -> char* { return data_; }
    [[nodiscard]] auto size() const -> std::size_t { return size_; }
    [[nodiscard]] auto empty() const -> bool { return size_ == 0; }
    [[nodiscard]] auto c_str() const -> char const*;
    [[nodiscard]] auto view() const -> std::string_view { return { data_, size_ }; }

    [[nodiscard]] auto intern(StringPool& pool) const -> StringId;

private:
    static constexpr std::uint32_t INLINE_CAP = 256;
    char buf_[INLINE_CAP] = {};
    char* data_ = buf_;
    std::uint32_t size_ = 0;
    std::uint32_t capacity_ = INLINE_CAP;

    auto is_heap() const -> bool { return data_ != buf_; }
    auto grow(std::size_t needed) -> void;
};

} // namespace pup
```

`src/core/buf.cpp`:
```cpp
// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/core/buf.hpp"
#include "pup/core/string_pool.hpp"

#include <cstdlib>
#include <cstring>

namespace pup {

Buf::~Buf()
{
    if (is_heap()) {
        std::free(data_);
    }
}

auto Buf::grow(std::size_t needed) -> void
{
    if (needed <= capacity_) {
        return;
    }
    auto new_cap = static_cast<std::size_t>(capacity_) + capacity_ / 2 + 16;
    if (new_cap < needed) {
        new_cap = needed;
    }
    if (is_heap()) {
        auto* p = static_cast<char*>(std::realloc(data_, new_cap));
        if (!p) {
            std::abort();
        }
        data_ = p;
    } else {
        auto* p = static_cast<char*>(std::malloc(new_cap));
        if (!p) {
            std::abort();
        }
        std::memcpy(p, buf_, size_);
        data_ = p;
    }
    capacity_ = static_cast<std::uint32_t>(new_cap);
}

auto Buf::append(std::string_view sv) -> void
{
    if (sv.empty()) {
        return;
    }
    auto new_size = size_ + sv.size();
    grow(new_size + 1);
    std::memcpy(data_ + size_, sv.data(), sv.size());
    size_ = static_cast<std::uint32_t>(new_size);
    data_[size_] = '\0';
}

auto Buf::append(char c) -> void
{
    grow(size_ + 2);
    data_[size_++] = c;
    data_[size_] = '\0';
}

auto Buf::operator+=(std::string_view sv) -> Buf&
{
    append(sv);
    return *this;
}

auto Buf::operator+=(char c) -> Buf&
{
    append(c);
    return *this;
}

auto Buf::reserve(std::size_t n) -> void
{
    grow(n + 1);
}

auto Buf::clear() -> void
{
    size_ = 0;
    data_[0] = '\0';
}

auto Buf::c_str() const -> char const*
{
    // Ensure null termination (data_ always has room due to +1 in grow)
    const_cast<char*>(data_)[size_] = '\0';
    return data_;
}

auto Buf::intern(StringPool& pool) const -> StringId
{
    return pool.intern(view());
}

} // namespace pup
```

Add to Tupfiles:
- `Tupfile`: `srcs-y += src/core/buf.cpp`
- `test/unit/Tupfile`: `test-srcs-y += test_buf.cpp`

- [ ] **Step 3: Build and run tests**

Run: `make && ./build/test/unit/putup_test '[buf]'`
Expected: All tests pass.

- [ ] **Step 4: Full suite, commit**

```bash
git add include/pup/core/buf.hpp src/core/buf.cpp \
        test/unit/test_buf.cpp Tupfile test/unit/Tupfile
git commit -m "Add Buf — 272-byte stack-first scratch buffer

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>"
```

---

### Task 3: Add fmt() method to Buf and HeapBuf

Move the format logic from `format_impl` into a shared header, then add `fmt()` as a method on both buffer types.

**Files:**
- Create: `include/pup/core/format_to.hpp` — shared format engine (template, writes to any appendable)
- Modify: `include/pup/core/buf.hpp` — add `fmt()` template method
- Modify: `include/pup/core/heap_buf.hpp` — add `fmt()` template method
- Modify: `test/unit/test_buf.cpp` — add fmt tests
- Modify: `test/unit/test_heap_buf.cpp` — add fmt tests

- [ ] **Step 1: Create format_to.hpp**

Extract the format engine into a header-only template that works with any type that has `append(string_view)` and `append(char)`:

```cpp
// include/pup/core/format_to.hpp
// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include <cassert>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace pup {

struct FormatArg {
    enum class Tag : std::uint8_t { StringView, Long, Char };

    Tag tag;
    union {
        std::string_view sv;
        long long ll;
        char c;
    };

    FormatArg(std::string_view s) : tag(Tag::StringView), sv(s) {}           // NOLINT
    FormatArg(char const* s) : tag(Tag::StringView), sv(s) {}                // NOLINT
    FormatArg(int v) : tag(Tag::Long), ll(v) {}                              // NOLINT
    FormatArg(long long v) : tag(Tag::Long), ll(v) {}                        // NOLINT
    FormatArg(unsigned int v) : tag(Tag::Long), ll(v) {}                     // NOLINT
    FormatArg(std::size_t v) : tag(Tag::Long), ll(static_cast<long long>(v)) {} // NOLINT
    FormatArg(char v) : tag(Tag::Char), c(v) {}                              // NOLINT
};

template<typename Buffer>
auto format_to(Buffer& out, std::string_view pattern, FormatArg const* args, std::size_t count) -> void
{
    auto arg_idx = std::size_t { 0 };
    auto pos = std::size_t { 0 };

    while (pos < pattern.size()) {
        auto open = pattern.find('{', pos);
        auto close_close = pattern.find("}}", pos);

        if (close_close != std::string_view::npos
            && (open == std::string_view::npos || close_close < open)) {
            out.append(pattern.substr(pos, close_close - pos));
            out.append('}');
            pos = close_close + 2;
            continue;
        }

        auto brace = open;
        if (brace == std::string_view::npos) {
            out.append(pattern.substr(pos));
            break;
        }

        out.append(pattern.substr(pos, brace - pos));

        if (brace + 1 < pattern.size() && pattern[brace + 1] == '{') {
            out.append('{');
            pos = brace + 2;
        } else if (brace + 1 < pattern.size() && pattern[brace + 1] == '}') {
            if (arg_idx < count) {
                auto const& arg = args[arg_idx++];
                switch (arg.tag) {
                case FormatArg::Tag::StringView:
                    out.append(arg.sv);
                    break;
                case FormatArg::Tag::Long: {
                    char buf[24];
                    auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), arg.ll);
                    out.append(std::string_view { buf, static_cast<std::size_t>(ptr - buf) });
                    break;
                }
                case FormatArg::Tag::Char:
                    out.append(arg.c);
                    break;
                }
            }
            pos = brace + 2;
        } else {
            out.append('{');
            pos = brace + 1;
        }
    }

    assert(arg_idx == count && "pup::fmt: argument count mismatch");
}

} // namespace pup
```

- [ ] **Step 2: Add fmt() method to both buffer headers**

In `buf.hpp`, add inside the class after `operator+=`:
```cpp
    template<typename... Args>
    auto fmt(std::string_view pattern, Args const&... args) -> void;

    auto fmt(std::string_view pattern) -> void;
```

In `buf.cpp`, add:
```cpp
#include "pup/core/format_to.hpp"

auto Buf::fmt(std::string_view pattern) -> void
{
    format_to(*this, pattern, nullptr, 0);
}
```

And in `buf.hpp` add the template definition (must be in header):
```cpp
// At end of file, after class definition but inside namespace pup:
template<typename... Args>
auto Buf::fmt(std::string_view pattern, Args const&... args) -> void
{
    FormatArg arg_array[] = { FormatArg(args)... };
    format_to(*this, pattern, arg_array, sizeof...(Args));
}
```

Same pattern for `HeapBuf`. The `#include "pup/core/format_to.hpp"` goes in both headers.

- [ ] **Step 3: Add fmt tests**

In `test_buf.cpp`:
```cpp
TEST_CASE("Buf fmt", "[buf]")
{
    auto buf = Buf {};
    buf.fmt("hello {} #{}", "world", 42);
    REQUIRE(buf.view() == "hello world #42");
}
```

In `test_heap_buf.cpp`:
```cpp
TEST_CASE("HeapBuf fmt", "[heap_buf]")
{
    auto buf = HeapBuf {};
    buf.fmt("error: {} at line {}", "syntax", 10);
    REQUIRE(buf.view() == "error: syntax at line 10");
}
```

- [ ] **Step 4: Build and test**

Run: `make && ./build/test/unit/putup_test '[buf]' && ./build/test/unit/putup_test '[heap_buf]'`
Expected: All pass including new fmt tests.

- [ ] **Step 5: Commit**

```bash
git add include/pup/core/format_to.hpp include/pup/core/buf.hpp \
        include/pup/core/heap_buf.hpp src/core/buf.cpp src/core/heap_buf.cpp \
        test/unit/test_buf.cpp test/unit/test_heap_buf.cpp
git commit -m "Add fmt() method to Buf and HeapBuf via shared format_to engine

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>"
```

---

### Task 4: global_pool() singleton

**Files:**
- Create: `include/pup/core/global_pool.hpp`
- Create: `src/core/global_pool.cpp`
- Modify: `Tupfile` — add `src/core/global_pool.cpp`

- [ ] **Step 1: Implement global_pool**

`include/pup/core/global_pool.hpp`:
```cpp
// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

namespace pup {

class StringPool;

auto global_pool() -> StringPool&;

} // namespace pup
```

`src/core/global_pool.cpp`:
```cpp
// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/core/global_pool.hpp"
#include "pup/core/string_pool.hpp"

namespace pup {

auto global_pool() -> StringPool&
{
    static StringPool pool;
    return pool;
}

} // namespace pup
```

Add `srcs-y += src/core/global_pool.cpp` to `Tupfile`.

- [ ] **Step 2: Build and test**

Run: `make && ./build/test/unit/putup_test`
Expected: All tests pass (global_pool is not called yet, just linked).

- [ ] **Step 3: Commit**

```bash
git add include/pup/core/global_pool.hpp src/core/global_pool.cpp Tupfile
git commit -m "Add global_pool() — process-wide StringPool singleton

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>"
```

---

## Phase 2: Struct Member Migration

### Task 5: Wire Graph::strings to global_pool

Before migrating struct members, make `Graph::strings` reference the global pool so existing code keeps working during migration.

**Files:**
- Modify: `include/pup/graph/dag.hpp`
- Modify: `src/graph/dag.cpp`

- [ ] **Step 1: Remove Graph::strings member, use global_pool()**

In `dag.hpp`, remove `StringPool strings;` from `struct Graph` and change `Graph::command_strings` similarly. All code that accesses `graph.strings` should call `global_pool()` instead.

In `dag.cpp`, update `make_graph()` and all free functions that reference `graph.strings` to use `global_pool()`.

The `BuildGraph::string_pool()` accessor returns `global_pool()`.
The `BuildGraph::intern()` calls `global_pool().intern()`.
The `BuildGraph::str()` calls `global_pool().get()`.

VarDb's `StringPool*` member points to `&global_pool()`.

- [ ] **Step 2: Clean build and test**

Run: `make clean && make && ./build/test/unit/putup_test`
Expected: All tests pass. Behavior identical — just using global pool instead of per-graph pool.

- [ ] **Step 3: Commit**

```bash
git add include/pup/graph/dag.hpp src/graph/dag.cpp src/cli/context.cpp \
        include/pup/parser/eval.hpp src/parser/eval.cpp
git commit -m "Wire Graph::strings to global_pool() singleton

Removes per-graph StringPool ownership. All string interning goes
through global_pool(). VarDb, Graph, BuildGraph all reference the
same singleton. Behavioral no-op — same dedup, same lifetimes.

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>"
```

---

### Task 6: Migrate struct members to StringId (batch by layer)

The largest task. Change `pup::String` members to `StringId` in headers, fix cascading errors in .cpp files. Work inside-out by layer.

**Files:** ~30 headers, ~20 .cpp files

This task is too large for a single commit. Break into sub-batches:

- [ ] **Step 1: Graph + Index layer**

Headers: `builder.hpp`, `rule_pattern.hpp`, `entry.hpp`, `dep_scanner.hpp`

For each `String` member, change to `StringId`. For each .cpp that reads the member, wrap with `global_pool().get(member)`. For each .cpp that writes the member, intern: `member = global_pool().intern(value)`.

Build: `make clean && make`
Test: `./build/test/unit/putup_test`
Commit when green.

- [ ] **Step 2: Parser layer**

Headers: `ast.hpp`, `eval.hpp`, `glob.hpp`, `ignore.hpp`, `var_tracking.hpp`, `parser.hpp`, `depfile.hpp`, `lexer.hpp`

`PatternFlags` fields become `std::string_view` (they're views into already-interned paths).
`EvalContext::tup_*` fields become `StringId`.
`Tupfile::filename` becomes `StringId`.
AST node string fields become `StringId`.

Build, test, commit.

- [ ] **Step 3: Exec + CLI + Core layer**

Headers: `scheduler.hpp`, `runner.hpp`, `progress_display.hpp`, `options.hpp`, `context.hpp`, `target.hpp`, `config_commands.hpp`, `result.hpp`, `layout.hpp`

`Error::message` becomes `StringId` — error messages are interned at construction, resolved at print.
`BuildJob` fields become `StringId`.
`Options` fields become `StringId`.
`ProjectLayout` fields become `StringId`.

Build, test, commit.

- [ ] **Step 4: Function return types**

Change functions that return `pup::String` to return `HeapBuf` (for built strings) or `StringId` (for interned results):

- `path::join`, `path::normalize`, `path::relative` → `HeapBuf`
- `expand_instruction` → `HeapBuf`
- `get_full_path` (convenience) → `HeapBuf`
- `hash_to_hex` → `HeapBuf`
- `shell_quote` → `HeapBuf`
- `get_command_string` → `HeapBuf`
- `fmt()` free function → deleted (use `Buf::fmt()`)
- `render_simple`, `format_duration` → `HeapBuf`

Build, test, commit.

---

## Phase 3: Remove pup::String

### Task 7: Delete pup::String and pup::fmt

Once all struct members are StringId and all builders use Buf/HeapBuf:

**Files:**
- Delete: `include/pup/core/string.hpp`
- Delete: `src/core/string.cpp`
- Delete: `include/pup/core/fmt.hpp`
- Delete: `src/core/fmt.cpp`
- Modify: `Tupfile` — remove `string.cpp`, `fmt.cpp`
- Modify: all files that `#include "pup/core/string.hpp"` → remove or replace with `string_id.hpp`
- Modify: all files that `#include "pup/core/fmt.hpp"` → remove or replace with `buf.hpp`

- [ ] **Step 1: Remove includes and typedefs**

Search for `#include "pup/core/string.hpp"` in all production files. Replace with `#include "pup/core/string_id.hpp"` (for StringId) or `#include "pup/core/buf.hpp"` / `#include "pup/core/heap_buf.hpp"` (for buffer usage).

Search for `using Path = String` in `path.hpp`. Remove it or change to `using Path = StringId`.

- [ ] **Step 2: Delete files and update Tupfile**

```bash
rm include/pup/core/string.hpp src/core/string.cpp
rm include/pup/core/fmt.hpp src/core/fmt.cpp
```

Remove from `Tupfile`:
```
srcs-y += src/core/string.cpp
srcs-y += src/core/fmt.cpp
```

Also remove from bootstrap scripts.

- [ ] **Step 3: Clean build and full test**

Run: `make clean && make && ./build/test/unit/putup_test`
Expected: All tests pass. Zero `pup::String` anywhere.

- [ ] **Step 4: Verify**

```bash
grep -rn "pup::String" src/ include/pup/ --include="*.hpp" --include="*.cpp" | grep -v "test/"
# Should be 0

size build/putup
# .text should be <= 563 KB
```

- [ ] **Step 5: Commit**

```bash
git add -u
git commit -m "Remove pup::String and pup::fmt — migration complete

All struct members use StringId. String building uses Buf (stack) or
HeapBuf (heap). pup::String and its SSO eliminated. fmt() replaced
by Buf::fmt() / HeapBuf::fmt().

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>"
```

---

## Execution Notes

**Clean builds:** Phase 2 changes header layouts. Use `make clean && make` after each header batch to avoid stale object files (the known ODR issue with putup's incremental build).

**Bootstrap scripts:** When `string.cpp` and `fmt.cpp` are deleted in Phase 3, remove them from `bootstrap-linux.sh`, `bootstrap-macos.sh`, `bootstrap-mingw.sh`. Add `buf.cpp`, `heap_buf.cpp`, `global_pool.cpp`.

**Test files:** Test code can keep using `pup::String` via `#include "pup/core/string.hpp"` during Phases 1-2. Only Phase 3 removes it — at that point, tests use `Buf`/`HeapBuf` for building and `StringId`/`string_view` for assertions.

**VarDb:** Already uses `SortedPairVec<StringId, StringId>` internally. Its `StringPool*` member changes to always point to `&global_pool()`. The constructor signature `VarDb(StringPool*)` can optionally be simplified to `VarDb()` (always uses global pool), but keeping the parameter is fine for testing with isolated pools.

**FormatArg constructors:** The `FormatArg(String const&)` constructor in current `fmt.hpp` is removed. `FormatArg(std::string_view)` handles all string arguments since both `StringId` (resolved to `string_view`) and buffer `view()` produce `string_view`.
