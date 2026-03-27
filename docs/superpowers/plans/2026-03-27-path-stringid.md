# Phase 3A: Path Functions Return StringId

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Change `path::join`, `path::normalize`, `path::relative`, and all `path_utils` functions from returning `pup::String` to returning `StringId` (interning internally). Update all 145 call sites across 19 files.

**Architecture:** Each path function builds the result in a local `Buf` (stack buffer), then interns via `global_pool().intern(buf.view())` and returns the `StringId`. Callers that need the string content use `pool.get(id)` to get a `string_view`. The `using Path = String` alias is removed.

**Tech Stack:** `pup::Buf` for building, `global_pool()` for interning, `StringId` for return type.

---

### Task 1: Change path.hpp signatures and path.cpp implementation

Change `join`, `normalize`, `relative` to return `StringId`. Replace internal `String` usage with `Buf`. Remove `using Path = String`.

**Files:**
- Modify: `include/pup/core/path.hpp`
- Modify: `src/core/path.cpp`

- [ ] **Step 1: Update path.hpp**

Replace the entire file content:

```cpp
// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include "pup/core/string_id.hpp"

#include <cstddef>
#include <string_view>

namespace pup {

/// Path string operations. All functions operate on forward-slash-separated
/// UTF-8 paths without touching the filesystem.
namespace path {

/// Join two path segments with '/'.
/// join("src", "foo.c") -> "src/foo.c"
/// join("src/", "foo.c") -> "src/foo.c"
/// join("", "foo.c") -> "foo.c"
/// join("src", "/usr/include") -> "/usr/include" (absolute rhs replaces)
[[nodiscard]]
auto join(std::string_view a, std::string_view b) -> StringId;

/// Get the parent directory.
/// parent("src/lib/foo.c") -> "src/lib"
/// parent("foo.c") -> ""
/// parent("") -> ""
/// parent("/") -> "/"
[[nodiscard]]
auto parent(std::string_view p) -> std::string_view;

/// Get the filename component (after last '/').
[[nodiscard]]
auto filename(std::string_view p) -> std::string_view;

/// Get the stem (filename without extension).
[[nodiscard]]
auto stem(std::string_view p) -> std::string_view;

/// Get the file extension (including dot).
[[nodiscard]]
auto extension(std::string_view p) -> std::string_view;

/// Check if a path is absolute.
[[nodiscard]]
auto is_absolute(std::string_view p) -> bool;

/// Lexically normalize a path by resolving '.' and '..' segments.
/// Does not touch the filesystem.
[[nodiscard]]
auto normalize(std::string_view p) -> StringId;

/// Compute relative path from base to target (lexical, no filesystem access).
[[nodiscard]]
auto relative(std::string_view target, std::string_view base) -> StringId;

} // namespace path
} // namespace pup
```

Key changes: `#include "string.hpp"` replaced with `#include "string_id.hpp"`. `using Path = String` removed. Three functions return `StringId` instead of `String`.

- [ ] **Step 2: Update path.cpp**

Replace the entire file content:

```cpp
// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/core/path.hpp"
#include "pup/core/buf.hpp"
#include "pup/core/global_pool.hpp"
#include "pup/core/string_pool.hpp"

#include <vector>

namespace pup::path {

auto join(std::string_view a, std::string_view b) -> StringId
{
    if (b.empty()) {
        return global_pool().intern(a);
    }
    if (a.empty() || is_absolute(b)) {
        return global_pool().intern(b);
    }

    auto buf = Buf {};
    buf.append(a);
    if (a.back() != '/') {
        buf += '/';
    }
    buf.append(b);
    return buf.intern(global_pool());
}

auto parent(std::string_view p) -> std::string_view
{
    if (p.empty()) {
        return {};
    }

    auto end = p.size();
    while (end > 1 && p[end - 1] == '/') {
        --end;
    }

    auto pos = p.rfind('/', end - 1);
    if (pos == std::string_view::npos) {
        return {};
    }
    if (pos == 0) {
        return p.substr(0, 1);
    }
    return p.substr(0, pos);
}

auto filename(std::string_view p) -> std::string_view
{
    if (p.empty()) {
        return {};
    }
    auto pos = p.rfind('/');
    if (pos == std::string_view::npos) {
        return p;
    }
    return p.substr(pos + 1);
}

auto stem(std::string_view p) -> std::string_view
{
    auto name = filename(p);
    if (name.empty() || name == "." || name == "..") {
        return name;
    }
    auto dot = name.rfind('.');
    if (dot == 0 || dot == std::string_view::npos) {
        return name;
    }
    return name.substr(0, dot);
}

auto extension(std::string_view p) -> std::string_view
{
    auto name = filename(p);
    if (name.empty() || name == "." || name == "..") {
        return {};
    }
    auto dot = name.rfind('.');
    if (dot == 0 || dot == std::string_view::npos) {
        return {};
    }
    return name.substr(dot);
}

auto is_absolute(std::string_view p) -> bool
{
    if (p.empty()) {
        return false;
    }
    if (p[0] == '/') {
        return true;
    }
#ifdef _WIN32
    if (p.size() >= 3 && p[1] == ':' && (p[2] == '/' || p[2] == '\\')) {
        auto c = p[0];
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
    }
#endif
    return false;
}

auto normalize(std::string_view p) -> StringId
{
    auto parts = std::vector<std::string_view> {};
    auto start = std::size_t { 0 };
    auto absolute = is_absolute(p);

    while (start < p.size()) {
        auto end = p.find('/', start);
        if (end == std::string_view::npos) {
            end = p.size();
        }
        auto part = p.substr(start, end - start);
        if (part.empty() || part == ".") {
            // skip
        } else if (part == ".." && !parts.empty() && parts.back() != "..") {
            parts.pop_back();
        } else if (part == ".." && absolute) {
            // Cannot go above root
        } else {
            parts.push_back(part);
        }
        start = end + 1;
    }

    auto& pool = global_pool();

    if (parts.empty()) {
        return pool.intern(absolute ? "/" : ".");
    }

    auto buf = Buf {};
    if (absolute) {
        buf += '/';
    }
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) {
            buf += '/';
        }
        buf.append(parts[i]);
    }
    return buf.intern(pool);
}

auto relative(std::string_view target, std::string_view base) -> StringId
{
    auto& pool = global_pool();

    if (target == base) {
        return pool.intern(".");
    }

    auto split = [](std::string_view p) {
        auto parts = std::vector<std::string_view> {};
        auto start = std::size_t { 0 };
        while (start < p.size()) {
            auto end = p.find('/', start);
            if (end == std::string_view::npos) {
                end = p.size();
            }
            auto part = p.substr(start, end - start);
            if (!part.empty() && part != ".") {
                parts.push_back(part);
            }
            start = end + 1;
        }
        return parts;
    };

    auto target_parts = split(target);
    auto base_parts = split(base);

    auto common = std::size_t { 0 };
    auto max_common = std::min(target_parts.size(), base_parts.size());
    while (common < max_common && target_parts[common] == base_parts[common]) {
        ++common;
    }

    auto buf = Buf {};
    for (auto i = common; i < base_parts.size(); ++i) {
        if (!buf.empty()) {
            buf += '/';
        }
        buf.append("..");
    }
    for (auto i = common; i < target_parts.size(); ++i) {
        if (!buf.empty()) {
            buf += '/';
        }
        buf.append(target_parts[i]);
    }

    if (buf.empty()) {
        return pool.intern(".");
    }
    return buf.intern(pool);
}

} // namespace pup::path
```

- [ ] **Step 3: Build to see all caller errors**

```bash
make 2>&1 | grep "error:" | head -30
```

This will show every file that needs updating. Do NOT fix them yet — this task only changes the interface.

- [ ] **Step 4: Commit the interface change (broken callers expected)**

```bash
git add include/pup/core/path.hpp src/core/path.cpp
git commit -m "Change path::join/normalize/relative to return StringId

Intern results via global_pool(). Build with Buf instead of String.
Remove 'using Path = String' alias. Callers will be fixed in subsequent commits."
```

---

### Task 2: Change path_utils functions to return StringId

**Files:**
- Modify: `include/pup/core/path_utils.hpp`
- Modify: `src/core/path_utils.cpp`

- [ ] **Step 1: Update path_utils.hpp**

Change all functions returning `String` to return `StringId`. Replace `#include "pup/core/string.hpp"` with `#include "pup/core/string_id.hpp"`. The functions are:
- `relative_to_root` -> `StringId`
- `compute_source_to_root` -> `StringId`
- `strip_path_prefix` -> `StringId`
- `resolve_under_root` -> `std::optional<StringId>`
- `make_source_relative` -> `StringId`

- [ ] **Step 2: Update path_utils.cpp**

Replace internal `String` usage with `Buf`, intern at the end. Read the current implementation, apply the same pattern as path.cpp: build in Buf, return `buf.intern(global_pool())`.

- [ ] **Step 3: Commit**

```bash
git add include/pup/core/path_utils.hpp src/core/path_utils.cpp
git commit -m "Change path_utils functions to return StringId"
```

---

### Task 3: Update layout.hpp inline methods

**Files:**
- Modify: `include/pup/core/layout.hpp`

- [ ] **Step 1: Change return types**

The five inline methods (`pup_dir`, `index_path`, `resolve_source`, `resolve_config`, `resolve_output`) currently return `String` because they call `path::join` which returned `String`. Now `path::join` returns `StringId`, so these methods return `StringId` naturally:

```cpp
[[nodiscard]]
auto pup_dir() const -> StringId
{
    return path::join(global_pool().get(output_root), ".pup");
}

[[nodiscard]]
auto index_path() const -> StringId
{
    return path::join(global_pool().get(pup_dir()), "index");
}

[[nodiscard]]
auto resolve_source(std::string_view rel) const -> StringId
{
    return path::join(global_pool().get(source_root), rel);
}

[[nodiscard]]
auto resolve_config(std::string_view rel) const -> StringId
{
    return path::join(global_pool().get(config_root), rel);
}

[[nodiscard]]
auto resolve_output(std::string_view rel) const -> StringId
{
    return path::join(global_pool().get(output_root), rel);
}
```

Note: `index_path()` calls `pup_dir()` which now returns StringId. It needs `pool.get()` to pass to `path::join`.

- [ ] **Step 2: Commit**

```bash
git add include/pup/core/layout.hpp
git commit -m "Change layout.hpp inline methods to return StringId"
```

---

### Task 4: Fix all callers — core and platform layer

Fix callers in `src/core/` and `src/platform/` that use path functions.

**Files:**
- Modify: `src/core/layout.cpp`
- Modify: `src/core/path_utils.cpp` (callers of path::join within)
- Modify: `src/platform/file_io-posix.cpp`
- Modify: `src/platform/file_io-win32.cpp`

**Transformation pattern:** Where code does:
```cpp
auto x = path::join(a, b);   // x was String, now StringId
use(x);                       // if use() takes string_view
```
Change to:
```cpp
auto x = path::join(a, b);
auto x_sv = pool.get(x);     // or global_pool().get(x)
use(x_sv);
```

If the result is only used once, inline: `pool.get(path::join(a, b))`.

If the result is stored as a struct member (already StringId), no `.get()` needed.

- [ ] **Step 1: Fix all callers in the listed files**

Read each file. Apply the transformation pattern. Ensure `#include "pup/core/global_pool.hpp"` and `#include "pup/core/string_pool.hpp"` are present where `pool.get()` is used.

- [ ] **Step 2: Build and verify**

```bash
make 2>&1 | grep "error:" | head -30
```

Remaining errors should only be in graph/, parser/, exec/, cli/ layers.

- [ ] **Step 3: Commit**

```bash
git add src/core/ src/platform/
git commit -m "Fix path StringId callers: core and platform layers"
```

---

### Task 5: Fix all callers — graph layer

**Files:**
- Modify: `src/graph/builder.cpp` (39 call sites — largest file)
- Modify: `src/graph/dag.cpp` (8 call sites)

- [ ] **Step 1: Fix all callers**

Same transformation pattern as Task 4. `builder.cpp` has many local helper functions that return `String` from path operations — these should also change to return `StringId`.

- [ ] **Step 2: Build and verify**

```bash
make 2>&1 | grep "error:" | head -30
```

- [ ] **Step 3: Commit**

```bash
git add src/graph/
git commit -m "Fix path StringId callers: graph layer"
```

---

### Task 6: Fix all callers — parser and exec layers

**Files:**
- Modify: `src/parser/eval.cpp` (1 call site)
- Modify: `src/exec/scheduler.cpp` (9 call sites)
- Modify: `src/exec/progress_display.cpp` (if affected)
- Modify: `src/exec/runner.cpp` (if affected)

- [ ] **Step 1: Fix all callers**

Same pattern. The scheduler has `resolve_variant_path` which returns `String` — change to return `StringId`.

- [ ] **Step 2: Build and verify**

```bash
make 2>&1 | grep "error:" | head -30
```

- [ ] **Step 3: Commit**

```bash
git add src/parser/ src/exec/
git commit -m "Fix path StringId callers: parser and exec layers"
```

---

### Task 7: Fix all callers — CLI layer

**Files:**
- Modify: `src/cli/context.cpp` (26 call sites — second largest)
- Modify: `src/cli/cmd_build.cpp` (9 call sites)
- Modify: `src/cli/cmd_clean.cpp` (5 call sites)
- Modify: `src/cli/cmd_show.cpp` (5 call sites)
- Modify: `src/cli/target.cpp` (6 call sites)
- Modify: `src/cli/cmd_parse.cpp` (2 call sites)
- Modify: `src/cli/cmd_configure.cpp` (3 call sites)
- Modify: `src/cli/config_commands.cpp` (1 call site)
- Modify: `src/cli/output.cpp` (1 call site)
- Modify: `src/cli/multi_variant.cpp` (1 call site)

- [ ] **Step 1: Fix all callers**

Same pattern. `context.cpp` has several local helper functions returning `String` that should change to `StringId`.

- [ ] **Step 2: Build successfully**

```bash
make 2>&1 | tail -5
```

Expected: Build succeeds with zero errors.

- [ ] **Step 3: Commit**

```bash
git add src/cli/
git commit -m "Fix path StringId callers: CLI layer"
```

---

### Task 8: Run tests and verify pool stats

- [ ] **Step 1: Run full test suite**

```bash
timeout 120 ./build/test/unit/putup_test 2>&1 | tail -3
```

Expected: All 464 tests pass.

- [ ] **Step 2: Check pool stats**

```bash
./build/putup --stat -B build 2>&1 | grep "String pool"
```

Record the new pool size. Compare with baseline (161K strings, 2.3 MB).

- [ ] **Step 3: Run format and tidy**

```bash
make format 2>&1 | tail -3
```

If formatting changes, commit them.

- [ ] **Step 4: Final commit (formatting if needed)**

```bash
git add -u
git commit -m "Apply clang-format after path StringId migration"
```
