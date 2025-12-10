# Pup C++ Style Guide

This document describes the coding conventions and style guidelines for the Pup project. These conventions are enforced through `.clang-format` and should be followed for all contributions.

## Table of Contents

1. [File Organization](#file-organization)
2. [Naming Conventions](#naming-conventions)
3. [Type Declarations](#type-declarations)
4. [Functions](#functions)
5. [Variables and Constants](#variables-and-constants)
6. [Memory Management](#memory-management)
7. [Error Handling](#error-handling)
8. [Control Flow](#control-flow)
9. [Comments and Documentation](#comments-and-documentation)
10. [Modern C++20 Idioms](#modern-c20-idioms)

## File Organization

### Header Files

```cpp
// SPDX-License-Identifier: MIT
// Copyright (c) 2024 pup authors

#pragma once

#include "pup/core/types.hpp"  // Project headers grouped together

#include <memory>              // Standard library headers
#include <string>
#include <vector>

namespace pup::parser {        // Nested namespaces

// Declarations...

} // namespace pup::parser     // Closing comment for namespace
```

### Implementation Files

```cpp
// SPDX-License-Identifier: MIT
// Copyright (c) 2024 pup authors

#include "pup/parser/lexer.hpp"  // Corresponding header first

#include "pup/core/hash.hpp"     // Other project headers

#include <algorithm>             // Standard library headers
#include <filesystem>

namespace pup::parser {          // Match header namespace

// Anonymous namespace for internal linkage
namespace {

auto helper_function() -> void { ... }
auto const INTERNAL_CONSTANT = 42;

} // namespace

// Public implementations...

} // namespace pup::parser
```

### Include Order

1. Corresponding header (for .cpp files)
2. Project headers from `pup/`
3. Third-party headers
4. Standard library headers

Group includes logically and separate groups with blank lines.

### File Naming

- Header files: `snake_case.hpp`
- Implementation files: `snake_case.cpp`
- Test files: `test_snake_case.cpp`

Examples: `lexer.hpp`, `eval.cpp`, `test_parser.cpp`

## Naming Conventions

### Types (Classes, Structs, Enums)

Use `PascalCase` for all type names:

```cpp
class BuildGraph { };
struct PathPattern { };
enum class ErrorCode { };
struct VarRef { };
```

### Functions and Methods

Use `snake_case` for all functions and methods:

```cpp
auto parse_tupfile() -> Result<Tupfile>;
auto add_node(Node node) -> Result<NodeId>;
auto get_or_create_file_node() -> Result<NodeId>;
```

### Variables

Use `snake_case` for all variables:

```cpp
auto node_count = std::size_t { 0 };
auto file_path = std::string { "foo.txt" };
auto current_dir = fs::path { "." };
```

### Member Variables

Use `snake_case` with trailing underscore for private member variables:

```cpp
class BuildGraph {
private:
    std::vector<Node> nodes_;
    NodeId next_id_ = 1;
    std::unordered_map<std::string, NodeId> path_index_;
};
```

### Constants

Use `SCREAMING_SNAKE_CASE` for constants in anonymous namespaces:

```cpp
namespace {
auto const MAGIC_VALUE = 42;
auto const DEFAULT_BUFFER_SIZE = std::size_t { 8192 };
}
```

For compile-time constants, use `snake_case` with `constexpr`:

```cpp
inline constexpr auto unit = Unit {};
static constexpr auto hex_chars = std::string_view { "0123456789abcdef" };
```

### Enum Values

Use `PascalCase` for enum values:

```cpp
enum class NodeType {
    File,
    Directory,
    Command,
    Generated,
};

enum class LinkType {
    Normal,
    Sticky,
    Group,
    Implicit,
};
```

### Namespaces

Use `snake_case` for namespaces:

```cpp
namespace pup::parser { }
namespace pup::graph { }
```

## Type Declarations

### AAA (Almost Always Auto)

Use `auto` with explicit type initialization for all declarations:

```cpp
// Literals - type is clear from value
auto x = 42;
auto pi = 3.14;
auto name = "hello";

// Function calls - wrap return value in explicit type
auto result = Result<void> { compute() };
auto node = Node { get_node() };
auto count = std::size_t { vec.size() };

// Factory functions that return the type are OK as-is
auto ptr = std::make_unique<Foo>();
auto opt = std::make_optional(42);

// References and pointers
auto const& ref = container;
auto* ptr = get_pointer();

// Explicit type when not obvious from RHS
auto result = std::string {};
auto vec = std::vector<int> { 1, 2, 3 };

// Iterators - wrap in explicit type
auto it = decltype(path_index_)::iterator { path_index_.find(key) };
auto it = decltype(groups)::const_iterator { groups.find(name) };
```

**Rationale**: The explicit type wrapper makes the type visible at the declaration site, improving readability while maintaining the benefits of `auto`.

### Right-side Const

Place `const` on the right side of the type:

```cpp
auto const& ref = value;           // Not: const auto& ref
int const* ptr;                    // Not: const int* ptr
std::string const& get_name();     // Not: const std::string& get_name()
```

### Trailing Return Types

Always use trailing return type syntax:

```cpp
auto foo() -> ReturnType;
auto bar(int x, std::string y) -> std::expected<Result, Error>;
auto get_node(NodeId id) -> Node*;
auto empty() const -> bool;
```

### Struct vs Class

Use `struct` for:
- POD types with public data members
- Types that are primarily data containers
- AST nodes, configuration objects

```cpp
struct Node {
    NodeId id = 0;
    NodeType type = NodeType::File;
    std::string path = {};
    std::vector<NodeId> inputs = {};
};

struct PathPattern {
    Expression path;
    bool is_exclusion = false;
    bool is_group = false;
};
```

Use `class` for:
- Types with significant encapsulation
- Types with invariants to maintain
- Types with private implementation details

```cpp
class BuildGraph {
public:
    auto add_node(Node node) -> Result<NodeId>;
    auto get_node(NodeId id) const -> Node const*;

private:
    std::vector<Node> nodes_;
    NodeId next_id_ = 1;
};
```

## Functions

### Function Declarations

```cpp
// Free functions
auto expand_inputs(BuilderContext& ctx, std::vector<PathPattern> const& patterns)
    -> Result<std::vector<std::string>>;

// Member functions
class Evaluator {
    auto expand(Expression const& expr) -> Result<std::string>;
    auto evaluate_condition(Conditional const& cond) -> bool;
};

// Const correctness
auto get_node(NodeId id) const -> Node const*;
auto find_by_path(std::string_view path) const -> std::optional<NodeId>;
```

### Attributes

Use `[[nodiscard]]` for functions that return values that should not be ignored:

```cpp
[[nodiscard]] auto add_node(Node node) -> Result<NodeId>;
[[nodiscard]] auto get_node(NodeId id) -> Node*;
[[nodiscard]] auto empty() const -> bool;
[[nodiscard]] auto is_literal() const -> bool;
```

### Parameter Passing

- Pass small types by value: `int`, `NodeId`, `bool`, `enum class`
- Pass strings by `std::string_view` when read-only
- Pass large objects by `const&` when read-only
- Pass output parameters by mutable reference or pointer
- Use `std::move` for sink parameters

```cpp
auto expand(std::string_view text) -> Result<std::string>;
auto process_rule(BuilderContext& ctx, parser::Rule const& rule) -> Result<void>;
auto add_node(Node node) -> Result<NodeId>;  // Takes ownership via move
```

## Variables and Constants

### Initialization

Always use brace initialization or explicit type construction:

```cpp
auto result = std::string {};
auto vec = std::vector<int> { 1, 2, 3 };
auto count = std::size_t { 0 };
auto node = Node {
    .type = NodeType::File,
    .path = "foo.c",
};
```

### Designated Initializers

Use designated initializers for structs to improve clarity:

```cpp
auto node = Node {
    .type = NodeType::Command,
    .command = "gcc -c foo.c",
    .display = "CC foo.c",
    .source_dir = "src/",
};

auto edge = Edge {
    .from = from_id,
    .to = to_id,
    .type = LinkType::Normal,
};
```

## Memory Management

### Smart Pointers

Prefer stack allocation. When dynamic allocation is needed:

```cpp
auto ptr = std::make_unique<Node>();
auto shared = std::make_shared<Data>();
```

### Rule of Zero/Five

Prefer the rule of zero (rely on compiler-generated special members). When you need custom implementations, implement all five:

```cpp
// Rule of Five example (from Sha256)
class Sha256 {
public:
    Sha256();
    ~Sha256();

    Sha256(Sha256&& other) noexcept;
    auto operator=(Sha256&& other) noexcept -> Sha256&;

    Sha256(Sha256 const&) = delete;
    auto operator=(Sha256 const&) = delete;
};
```

### RAII

Use RAII for resource management:

```cpp
auto file = std::ifstream { path, std::ios::binary };
if (!file)
    return make_error<Hash256>(ErrorCode::IoError, "Failed to open file");

// File automatically closed when scope exits
```

## Error Handling

### Result Type

Use `Result<T>` for operations that may fail:

```cpp
auto parse() -> Result<Tupfile>;
auto expand(Expression const& expr) -> Result<std::string>;
auto add_node(Node node) -> Result<NodeId>;
```

### Error Propagation

Use early returns to propagate errors:

```cpp
auto process_rule(BuilderContext& ctx, Rule const& rule) -> Result<void>
{
    auto inputs = Result<std::vector<std::string>> { expand_inputs(ctx, rule.inputs) };
    if (!inputs)
        return pup::unexpected<Error>(inputs.error());

    auto outputs = Result<std::vector<std::string>> { expand_outputs(ctx, rule.outputs, inputs->at(0)) };
    if (!outputs)
        return pup::unexpected<Error>(outputs.error());

    return {};
}
```

### Creating Errors

Use `make_error` helper for creating error results:

```cpp
if (!fs::exists(path))
    return make_error<void>(ErrorCode::NotFound, "File not found: " + path.string());

if (id >= nodes_.size())
    return make_error<void>(ErrorCode::InvalidNodeId, "Invalid node ID");
```

### Success Values

Use empty braces `{}` for successful `Result<void>`:

```cpp
auto do_work() -> Result<void>
{
    // ... do work ...
    return {};  // Success
}
```

## Control Flow

### Braces

Single-statement bodies do not require braces (WebKit style):

```cpp
if (condition)
    return early;

for (auto const& item : items)
    process(item);

while (pos < text.size())
    ++pos;
```

Multi-statement bodies require braces:

```cpp
if (condition) {
    do_something();
    do_something_else();
}

for (auto const& item : items) {
    auto result = process(item);
    if (!result)
        return result;
}
```

### Early Returns

Prefer early returns over nested conditions:

```cpp
// Good
auto get_node(NodeId id) -> Node*
{
    if (id == 0 || id >= nodes_.size())
        return nullptr;
    auto& node = nodes_[id];
    return node.id == id ? &node : nullptr;
}

// Avoid
auto get_node(NodeId id) -> Node*
{
    if (id != 0 && id < nodes_.size()) {
        auto& node = nodes_[id];
        if (node.id == id)
            return &node;
    }
    return nullptr;
}
```

### Range-based For Loops

Prefer range-based for loops:

```cpp
for (auto const& stmt : tupfile.statements) {
    auto result = process_statement(ctx, *stmt);
    if (!result)
        return pup::unexpected<Error>(result.error());
}

for (auto const& [name, value] : vars_)
    process(name, value);
```

### Switch Statements

Use `switch` for enums, ensure all cases are covered:

```cpp
switch (assign.op) {
case Assignment::Op::Set:
    db->set(*name, *value);
    break;
case Assignment::Op::Append:
    db->append(*name, *value);
    break;
case Assignment::Op::Define:
    db->set(*name, *value);
    break;
}
```

## Comments and Documentation

### Comment Philosophy

Code should be self-documenting. Only add comments when:
- The logic is complex or non-obvious
- Explaining the "why" rather than the "what"
- Documenting important invariants or assumptions

```cpp
// Good - explains why
// Recursively expand any variable references that were embedded in literals
// (e.g., from escaped quotes like \"$(VAR)\")
return expand(std::string_view { result });

// Avoid - states the obvious
// Loop through all nodes
for (auto const& node : nodes_) { ... }
```

### File Headers

Every file should have a license header:

```cpp
// SPDX-License-Identifier: MIT
// Copyright (c) 2024 pup authors
```

### Struct/Class Documentation

Document complex types with brief descriptions:

```cpp
/// Base for all AST nodes with source location tracking
struct AstNode {
    SourceLocation location;
};

/// Build graph - DAG of nodes and edges
class BuildGraph {
    // ...
};
```

### Member Documentation

Document non-obvious members:

```cpp
struct Node {
    NodeId id = 0;
    NodeType type = NodeType::File;

    std::string path = {};       ///< For files: relative path from tup root
    std::string command = {};    ///< For commands: the command string
    std::string source_dir = {}; ///< For commands: Tupfile directory
};
```

### Implementation Comments

Use comments to explain complex algorithms or non-obvious logic:

```cpp
// Strip trailing slashes from a path string
auto strip_trailing_slashes(std::string str) -> std::string
{
    while (!str.empty() && (str.back() == '/' || str.back() == '\\'))
        str.pop_back();
    return str;
}

// Use NodeId as vector index for O(1) lookup
if (id >= nodes_.size())
    nodes_.resize(id + 1);
```

### Section Separators

Use section separators for logical grouping in implementation files:

```cpp
// =============================================================================
// VarDb
// =============================================================================

auto VarDb::set(std::string_view name, std::string value) -> void
{
    // ...
}

// =============================================================================
// Evaluator
// =============================================================================

Evaluator::Evaluator(EvalContext& ctx)
    : ctx_(ctx)
{
}
```

## Modern C++20 Idioms

### Concepts and Constraints

Use concepts for template constraints:

```cpp
template<typename T, typename Msg>
requires std::is_convertible_v<Msg, std::string_view>
[[nodiscard]] auto make_error(ErrorCode code, Msg&& msg) -> Result<T>
{
    return pup::unexpected<Error>(Error::make(code, std::string { std::forward<Msg>(msg) }));
}
```

### std::span

Use `std::span` for non-owning array views:

```cpp
auto update(std::span<std::byte const> data) -> void;
```

### std::optional

Use `std::optional` for optional values:

```cpp
[[nodiscard]] auto find_by_path(std::string_view path) const -> std::optional<NodeId>;

std::optional<Expression> display;
std::optional<std::string> output_group;
```

### std::variant

Use `std::variant` for type-safe unions:

```cpp
struct Expression {
    struct Literal { std::string value; };
    struct Variable { VarRef ref; };

    std::vector<std::variant<Literal, Variable>> parts;
};
```

### std::filesystem

Use `std::filesystem` for path operations:

```cpp
namespace fs = std::filesystem;

auto normalize_path(std::string const& path_str) -> std::string
{
    auto path = fs::path { path_str }.lexically_normal();
    return path.string();
}
```

### Structured Bindings

Use structured bindings for decomposing values:

```cpp
for (auto const& [name, value] : vars_)
    result.push_back(name);
```

### Lambda Functions

Use lambdas for callbacks and local functions:

```cpp
eval.resolve_group = [&ctx](std::string_view name) -> std::vector<std::string> {
    auto it = ctx.groups.find(std::string { name });
    if (it == ctx.groups.end())
        return {};
    // ...
};

auto parse_nibble = [](char c) -> int {
    if (c >= '0' && c <= '9')
        return c - '0';
    return -1;
};
```

## Internal Linkage

Use anonymous namespaces for internal linkage instead of `static`:

```cpp
namespace {

auto helper_function() -> void { ... }
auto const MAGIC_VALUE = 42;

} // namespace
```

**Exception**: `static` is acceptable for:
- `static constexpr` compile-time constants inside functions
- `static` member functions

## Formatting

The project uses `.clang-format` with WebKit style. Key formatting rules:

- Indentation: 4 spaces
- Namespace contents: not indented
- Line length: 120 characters (soft limit)
- Braces: on same line for control flow, new line for functions
- No braces for single statements

Run formatting with:
```bash
make format
```

Check formatting with:
```bash
make format-check
```

## Summary Checklist

When writing code, ensure:

- [ ] File has SPDX license header
- [ ] Includes are ordered correctly
- [ ] Names follow conventions (PascalCase types, snake_case functions/vars)
- [ ] Using `auto` with explicit type initialization
- [ ] Using trailing return types
- [ ] Using `const` on the right side
- [ ] Using `[[nodiscard]]` for important return values
- [ ] Using `Result<T>` for fallible operations
- [ ] Early returns for error cases
- [ ] Anonymous namespace for internal helpers
- [ ] Comments explain "why", not "what"
- [ ] Code is self-documenting with clear names
- [ ] Formatted with `make format`
