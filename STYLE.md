# Modern C++ Style Guide

**Version:** 1.8.0
**Updated:** 2025-12-20
**C++ Standard:** C++20/23

A concise style guide for modern C++ projects.

## Changelog

| Version | Date | Changes |
|---------|------|---------|
| 1.8.0 | 2025-12-20 | Null pointer initialization: use static_cast for AAA |
| 1.7.0 | 2025-12-18 | Attributes on their own line |
| 1.6.0 | 2025-12-18 | Multiline lambdas: same formatting as functions |
| 1.5.0 | 2025-12-16 | Multiline declarations: return type follows closing paren |
| 1.4.0 | 2025-12-14 | Remove explicit type wrapper requirement for function calls |
| 1.3.0 | 2025-12-10 | Always use braces for if/for/while statements |
| 1.2.0 | 2025-12-10 | Call site arguments follow same formatting as declarations |
| 1.1.0 | 2025-12-10 | Long parameter lists: one parameter per line |
| 1.0.0 | 2024-12-10 | Initial release |

## Core Principles

1. **Clarity over brevity** - Readable code, clear intent
2. **Const by default** - Right-side const, immutable when possible
3. **Early returns** - Reduce nesting, handle errors immediately
4. **Self-documenting code** - Clear names over comments

## Project Primitives

This project builds without libstdc++ (`-nostdlib++`), so container and string
storage use in-house primitives instead of the STL equivalents:

| Standard type | Project primitive | Role |
|---------------|-------------------|------|
| `std::vector<T>` | `Vec<T>` (`SortedPairVec` for sorted key/value) | Owning sequence |
| `std::string` | `StringId`/`StringPool` (storage), `Buf` (building) | Owned text |
| `std::function<Sig>` | `Function<Sig>` | Type-erased callable |

Header-only views and value types from the standard library are used directly:
`std::string_view` (read-only text), `std::optional`, `std::variant`, `std::span`.

## Type Declarations

### AAA (Almost Always Auto)

```cpp
// Literals - type clear from value
auto x = 42;
auto pi = 3.14;
auto name = "hello";

// Function calls - auto deduces return type
auto result = compute();
auto count = vec.size();

// Factory functions
auto ptr = std::make_unique<Foo>();
auto opt = std::make_optional(42);

// Collections with initializers
auto ids = Vec<int>{ 1, 2, 3 };

// References and pointers
auto const& ref = container;
auto* ptr = get_pointer();

// Null pointer initialization - use static_cast for type
auto const* ptr = static_cast<Foo const*>(nullptr);
```

### Trailing Return Types

```cpp
// Short declarations - return type on same line
auto foo() -> ReturnType;
auto empty() const -> bool;

// Multiline declarations/definitions - closing paren on own line, return type follows
auto create_connection(
    std::string_view host,
    std::uint16_t port,
    ConnectionOptions const& options
) -> Result<Connection>;
```

### Right-side Const

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

#pragma once                        // Header guard

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

## Functions

```cpp
// Use [[nodiscard]] for values that shouldn't be ignored
[[nodiscard]]
auto compute() -> Result<int>;

// Parameter passing:
// - Small types by value: int, bool, enum, NodeId
// - Read-only strings: std::string_view
// - Read-only large objects: const&
// - Sink parameters: by value, move into place
auto process(std::string_view text) -> Result<void>;
auto add(Node node) -> Result<NodeId>;  // takes ownership

// Long parameter lists - one parameter per line
auto create_connection(
    std::string_view host,
    std::uint16_t port,
    ConnectionOptions const& options,
    std::chrono::milliseconds timeout
) -> Result<Connection>;

// Fits on one line - keep together
auto add(int a, int b) -> int;

// Call sites follow the same pattern
auto conn = create_connection(
    "localhost",
    8080,
    default_options,
    std::chrono::milliseconds{5000}
);
```

## Control Flow

```cpp
// Always use braces
if (condition) {
    return early;
}

for (auto const& item : items) {
    process(item);
}

// Early returns over nesting
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

```cpp
// Result<T> for fallible operations
auto parse() -> Result<Config>;

// Create a new error at the failure site with make_error<T>
auto load(std::string_view path) -> Result<Config>
{
    if (path.empty()) {
        return make_error<Config>(ErrorCode::InvalidArgument, "path is empty");
    }
    return parse();
}

// Propagate an existing error unchanged: unexpected<Error>(x.error())
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

## Modern C++ Idioms

```cpp
// Designated initializers
auto node = Node{
    .type = NodeType::File,
    .path = "foo.c",
};

// Structured bindings
for (auto const& [key, value] : map) {
    process(key, value);
}

// std::optional for optional values
auto find(Key k) -> std::optional<Value>;

// std::variant for type-safe unions
using Token = std::variant<Literal, Identifier, Operator>;

// std::span for non-owning views
auto process(std::span<std::byte const> data) -> void;

// Lambdas - short form
auto pred = [&ctx](auto const& item) { return item.matches(ctx); };

// Lambdas - multiline (same formatting as functions)
auto create_file = [&](
    std::string_view path,
    std::string_view name
) -> NodeId {
    // ...
};

// Concepts (C++20)
template<typename T>
requires std::integral<T>
auto count_bits(T value) -> int;

// ranges (C++20/23)
auto names = items | std::views::filter(is_valid)
                   | std::views::transform(&Item::name);
```

## Comments

Only when explaining **why**, not **what**:

```cpp
// Good - explains non-obvious reason
// Recursively expand to handle nested variable refs like \"$(VAR)\"
return expand(std::string_view{result});

// Bad - states the obvious
// Loop through all nodes
for (auto const& node : nodes_) { }
```

## Struct vs Class

**struct**: Public data, minimal logic, POD-like
```cpp
struct Point { int x; int y; };
struct Config { std::string path; int timeout = 30; };
```

**class**: Encapsulation, invariants, private state
```cpp
class Graph {
public:
    auto add_node(Node n) -> NodeId;
private:
    Vec<Node> nodes_;
};
```

## Checklist

- [ ] Use `auto` for variable declarations
- [ ] Trailing return types
- [ ] Right-side `const`
- [ ] `[[nodiscard]]` on important returns
- [ ] Anonymous namespace for internal linkage
- [ ] Early returns for error cases
- [ ] Long parameter/argument lists formatted one-per-line
- [ ] Always use braces for control flow statements
- [ ] Comments explain "why" not "what"
