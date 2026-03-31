# Code Style Reference

C++20/23 style guide for putup. WebKit-based formatting via `.clang-format`.

## Core Principles

1. Clarity over brevity
2. Const by default (right-side const)
3. Early returns to reduce nesting
4. Self-documenting code over comments

## AAA (Almost Always Auto)

```cpp
auto x = 42;
auto pi = 3.14;
auto result = compute();
auto ptr = std::make_unique<Foo>();
auto vec = std::vector<int>{1, 2, 3};
auto const& ref = container;
auto* raw = get_pointer();

// Null pointer -- use static_cast for type
auto const* p = static_cast<Foo const*>(nullptr);
```

## Trailing Return Types

```cpp
// Short -- same line
auto foo() -> ReturnType;
auto empty() const -> bool;

// Long -- closing paren on own line, return type follows
auto create_connection(
    std::string_view host,
    std::uint16_t port,
    ConnectionOptions const& options
) -> Result<Connection>;
```

## Right-Side Const

```cpp
auto const& ref = value;           // Not: const auto& ref
int const* ptr;                    // Not: const int* ptr
std::string const& name() const;   // Not: const std::string&
```

## Naming

| Element | Style | Example |
|---------|-------|---------|
| Types (class, struct, enum) | `PascalCase` | `BuildGraph`, `NodeType` |
| Functions, methods | `snake_case` | `parse_file()`, `add_node()` |
| Variables | `snake_case` | `node_count`, `file_path` |
| Private members | `snake_case_` | `nodes_`, `next_id_` |
| Anonymous namespace constants | `SCREAMING_CASE` | `MAGIC_VALUE` |
| Compile-time constants | `snake_case` | `constexpr auto max_size = 64;` |
| Enum values | `PascalCase` | `NodeType::Command` |
| Namespaces | `snake_case` | `namespace my_project` |

## File Organization

```cpp
// SPDX-License-Identifier: MIT
// Copyright (c) 2024 authors

#pragma once

#include "project/module/foo.hpp"   // 1. Corresponding header (.cpp)
                                    // 2. Project headers
#include <memory>                   // 3. Standard library
#include <string>

namespace project::module {

// Anonymous namespace for internal linkage (not static)
namespace {
auto helper() -> void { }
auto const INTERNAL_CONST = 42;
}

// Public interface...

} // namespace project::module
```

## Parameters

Pass small types by value, strings as `string_view`, large objects as `const&`, sinks by value (then move). Long parameter lists go one-per-line; call sites follow the same pattern:

```cpp
auto create_connection(
    std::string_view host,
    std::uint16_t port,
    ConnectionOptions const& options
) -> Result<Connection>;
```

## Control Flow

Always use braces. Prefer early returns over nesting:

```cpp
auto get_node(NodeId id) -> Node*
{
    if (id == 0) {
        return nullptr;
    }
    if (id >= nodes_.size()) {
        return nullptr;
    }
    return &nodes_[id];
}
```

## Error Handling

Use `Result<T>` for fallible operations. Propagate with early returns:

```cpp
auto process(Path const& path) -> Result<void>
{
    auto content = read_file(path);
    if (!content) {
        return unexpected<Error>(content.error());
    }

    auto parsed = parse(*content);
    if (!parsed) {
        return unexpected<Error>(parsed.error());
    }

    return {};  // Success for Result<void>
}
```

## Modern Idioms

```cpp
// Designated initializers
auto node = Node{ .type = NodeType::File, .path = "foo.c" };

// Structured bindings
for (auto const& [key, value] : map) { process(key, value); }

// Lambdas -- short: inline; multiline: same formatting as functions
auto pred = [&ctx](auto const& item) { return item.matches(ctx); };
auto create_file = [&](std::string_view path, std::string_view name) -> NodeId {
    // ...
};

// [[nodiscard]] for values that must not be ignored
[[nodiscard]] auto compute() -> Result<int>;

// Concepts (C++20)
template<typename T> requires std::integral<T>
auto count_bits(T value) -> int;
```

## Struct vs Class

**struct** for public data, minimal logic. **class** for encapsulation and invariants:

```cpp
struct Config { std::string path; int timeout = 30; };

class Graph {
public:
    auto add_node(Node n) -> NodeId;
private:
    std::vector<Node> nodes_;
};
```

## Comments

Only explain **why**, never **what**. Use `//FIXME` for unavoidable workarounds caused by upstream issues.

## Checklist

- [ ] `auto` for variable declarations
- [ ] Trailing return types on all functions
- [ ] Right-side `const`
- [ ] `[[nodiscard]]` on important returns
- [ ] Anonymous namespace for internal linkage (not `static`)
- [ ] Early returns for error cases
- [ ] Long parameter/argument lists formatted one-per-line
- [ ] Always braces for if/for/while
- [ ] Comments explain "why" not "what"
- [ ] `make format` passes
- [ ] `make tidy` passes
