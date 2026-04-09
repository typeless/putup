# Thread PathId Through CommandInfo and Scanner Registries — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace `Vec<StringId>` with `Vec<PathId>` in `CommandInfo.outputs` and the `expand_outputs` pipeline, deferring path materialization to system boundaries.

**Architecture:** `expand_outputs` becomes a pure path computation (no node creation). Scanners match against the instruction pattern (with `%o`/`%f` placeholders). A new `ensure_file_node` creates nodes from `PathId` without string materialization. Display text is the sole graph-building-time materialization site.

**Tech Stack:** C++20, Catch2 (BDD-style), putup build system

**Spec:** `docs/superpowers/specs/2026-04-09-pathid-commandinfo-design.md`

---

## File Map

| File | Action | Responsibility |
|------|--------|----------------|
| `include/pup/core/buf.hpp` | Modify | Increase `INLINE_CAP` 256 → 4096 |
| `include/pup/core/path_pool.hpp` | Modify | Add `write(PathId, Buf&)` declaration |
| `src/core/path_pool.cpp` | Modify | Implement `write(PathId, Buf&)` |
| `test/unit/test_path_pool.cpp` | Modify | Add tests for `write` |
| `include/pup/graph/dag.hpp` | Modify | Add `ensure_file_node` declaration |
| `src/graph/dag.cpp` | Modify | Implement `ensure_file_node` |
| `test/unit/test_graph.cpp` | Modify | Add tests for `ensure_file_node` |
| `include/pup/graph/rule_pattern.hpp` | Modify | Change `CommandInfo.outputs` to `Vec<PathId>`, add `path_id.hpp` include |
| `src/graph/scanners/gcc.cpp` | Modify | Get source files from `cmd.inputs` in `build_dep_command` |
| `test/unit/test_dep_scanner.cpp` | Modify | Update 22 `CommandInfo` constructions |
| `test/unit/test_rule_pattern.cpp` | Modify | Update 43 `CommandInfo` constructions |
| `src/graph/builder.cpp` | Modify | Rewrite `expand_outputs`, split `expand_command`, update output edge loop |

---

### Task 1: `PathPool::write` — materialize into `Buf` without interning

**Files:**
- Modify: `include/pup/core/path_pool.hpp:52-54`
- Modify: `src/core/path_pool.cpp:84-111`
- Modify: `test/unit/test_path_pool.cpp:164-195`

- [ ] **Step 1: Write failing tests for `PathPool::write`**

Add a new `TEST_CASE` after the existing `to_string` tests in `test/unit/test_path_pool.cpp`:

```cpp
TEST_CASE("PathPool write materializes into Buf", "[path_pool]")
{
    auto pool = PathPool {};
    auto& sp = global_pool();

    SECTION("root writes empty")
    {
        auto buf = Buf {};
        pool.write(PathId::Root, buf, sp);
        REQUIRE(buf.view() == "");
    }

    SECTION("single component")
    {
        auto id = pool.intern_path("src", sp);
        auto buf = Buf {};
        pool.write(id, buf, sp);
        REQUIRE(buf.view() == "src");
    }

    SECTION("multi-component path")
    {
        auto id = pool.intern_path("src/lib/foo.c", sp);
        auto buf = Buf {};
        pool.write(id, buf, sp);
        REQUIRE(buf.view() == "src/lib/foo.c");
    }

    SECTION("write appends to existing buffer content")
    {
        auto id = pool.intern_path("foo.o", sp);
        auto buf = Buf {};
        buf += "output: ";
        pool.write(id, buf, sp);
        REQUIRE(buf.view() == "output: foo.o");
    }

    SECTION("matches to_string output")
    {
        auto paths = { "a", "a/b", "a/b/c/d/e.txt", "Makefile" };
        for (auto p : paths) {
            auto id = pool.intern_path(p, sp);
            auto buf = Buf {};
            pool.write(id, buf, sp);
            REQUIRE(buf.view() == sv(pool.to_string(id, sp)));
        }
    }
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `./build/test/unit/putup_test "[path_pool]" -s 2>&1 | tail -5`
Expected: Compilation error — `write` method not declared.

- [ ] **Step 3: Add `write` declaration to `path_pool.hpp`**

Add after the `to_string` declaration at line 54 in `include/pup/core/path_pool.hpp`:

```cpp
    /// Materialize: write full path into a Buf. No interning — for transient use.
    auto write(PathId id, Buf& buf, StringPool const& pool) const -> void;
```

Also add forward declaration at top of file (after `class StringPool;` is already there — no change needed, but add `#include "pup/core/buf.hpp"` is NOT needed in the header; forward-declare `Buf` instead):

Add before `class PathPool final {`:
```cpp
class Buf;
```

- [ ] **Step 4: Implement `write` in `path_pool.cpp`**

Add after the `to_string` implementation (after line 111) in `src/core/path_pool.cpp`:

```cpp
auto PathPool::write(PathId id, Buf& buf, StringPool const& pool) const -> void
{
    if (is_root(id)) {
        return;
    }

    auto const& entry = entries_[to_underlying(id)];
    if (is_root(entry.parent)) {
        buf.append(pool.get(entry.name));
        return;
    }

    auto stack = Vec<StringId> {};
    auto cur = id;
    while (!is_root(cur)) {
        stack.push_back(entries_[to_underlying(cur)].name);
        cur = entries_[to_underlying(cur)].parent;
    }

    for (auto i = stack.size(); i > 0; --i) {
        if (i < stack.size()) {
            buf += '/';
        }
        buf.append(pool.get(stack[i - 1]));
    }
}
```

- [ ] **Step 5: Build and run tests**

Run: `make && ./build/test/unit/putup_test "[path_pool]" -s 2>&1 | tail -10`
Expected: All path_pool tests PASS.

- [ ] **Step 6: Commit**

```bash
git add include/pup/core/path_pool.hpp src/core/path_pool.cpp test/unit/test_path_pool.cpp
git commit -m "Add PathPool::write for Buf materialization without interning"
```

---

### Task 2: Increase `Buf::INLINE_CAP` to 4096

**Files:**
- Modify: `include/pup/core/buf.hpp:76`

- [ ] **Step 1: Change INLINE_CAP**

In `include/pup/core/buf.hpp`, change line 76:

```cpp
    static constexpr std::uint32_t INLINE_CAP = 4096;
```

- [ ] **Step 2: Build and run all tests**

Run: `make && make test 2>&1 | tail -5`
Expected: All tests PASS. This is a size-only change — no behavior difference.

- [ ] **Step 3: Commit**

```bash
git add include/pup/core/buf.hpp
git commit -m "Increase Buf::INLINE_CAP from 256 to 4096"
```

---

### Task 3: `ensure_file_node` — PathId-native node creation

**Files:**
- Modify: `include/pup/graph/dag.hpp:143`
- Modify: `src/graph/dag.cpp`
- Modify: `test/unit/test_graph.cpp`

- [ ] **Step 1: Write failing tests for `ensure_file_node`**

Add a new `TEST_CASE` in `test/unit/test_graph.cpp`. First check existing includes and test patterns at the top of the file to follow the same style. Add:

```cpp
TEST_CASE("ensure_file_node creates nodes from PathId", "[graph]")
{
    auto bs = make_build_state();
    auto& graph = bs.graph;
    auto& pool = global_pool();

    // Set up build root so path_to_node is populated
    set_build_root_name(graph, intern("build"));

    SECTION("creates new file node under build root")
    {
        auto path_id = graph.paths.intern_path("build/foo.o", pool);
        auto result = ensure_file_node(graph, path_id, NodeType::Generated);
        REQUIRE(result.has_value());

        auto const* node = get_file_node(graph, *result);
        REQUIRE(node != nullptr);
        REQUIRE(node->type == NodeType::Generated);
        REQUIRE(node->name == intern("foo.o"));
    }

    SECTION("returns existing node on second call")
    {
        auto path_id = graph.paths.intern_path("build/bar.o", pool);
        auto first = ensure_file_node(graph, path_id, NodeType::Generated);
        auto second = ensure_file_node(graph, path_id, NodeType::Generated);
        REQUIRE(first.has_value());
        REQUIRE(second.has_value());
        REQUIRE(*first == *second);
    }

    SECTION("creates intermediate directories")
    {
        auto path_id = graph.paths.intern_path("build/src/lib/baz.o", pool);
        auto result = ensure_file_node(graph, path_id, NodeType::Generated);
        REQUIRE(result.has_value());

        auto const* node = get_file_node(graph, *result);
        REQUIRE(node != nullptr);
        REQUIRE(node->name == intern("baz.o"));

        // Parent chain should exist
        auto const* parent = get_file_node(graph, node->parent_dir);
        REQUIRE(parent != nullptr);
        REQUIRE(parent->name == intern("lib"));
        REQUIRE(parent->type == NodeType::Directory);
    }

    SECTION("upgrades Ghost to Generated")
    {
        auto path_id = graph.paths.intern_path("build/ghost.o", pool);

        // Create as Ghost first
        auto ghost_result = ensure_file_node(graph, path_id, NodeType::Ghost);
        REQUIRE(ghost_result.has_value());
        REQUIRE(get_file_node(graph, *ghost_result)->type == NodeType::Ghost);

        // Upgrade to Generated
        auto gen_result = ensure_file_node(graph, path_id, NodeType::Generated);
        REQUIRE(gen_result.has_value());
        REQUIRE(*gen_result == *ghost_result); // Same node
        REQUIRE(get_file_node(graph, *gen_result)->type == NodeType::Generated);
    }

    SECTION("path_to_node is consistent after creation")
    {
        auto path_id = graph.paths.intern_path("build/check.o", pool);
        auto result = ensure_file_node(graph, path_id, NodeType::Generated);
        REQUIRE(result.has_value());

        // path_to_node should map path_id -> node_id
        auto const* found = graph.path_to_node.find(to_underlying(path_id));
        REQUIRE(found != nullptr);
        REQUIRE(*found == *result);
    }
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `make 2>&1 | tail -5`
Expected: Compilation error — `ensure_file_node` not declared.

- [ ] **Step 3: Add declaration to `dag.hpp`**

Add after the `add_file_node` declaration (after line 143) in `include/pup/graph/dag.hpp`:

```cpp
/// Create or find a file node from a PathId, walking the PathPool trie.
/// Creates intermediate directory nodes as needed.
/// Handles type upgrade (Ghost/File -> Generated).
[[nodiscard]]
auto ensure_file_node(Graph& graph, PathId path_id, NodeType type) -> Result<NodeId>;
```

Add `#include "pup/core/path_id.hpp"` to the includes if not already present. Check the existing includes first — dag.hpp may already include it transitively.

- [ ] **Step 4: Implement `ensure_file_node` in `dag.cpp`**

Add after the `add_file_node` implementation in `src/graph/dag.cpp`:

```cpp
auto ensure_file_node(Graph& graph, PathId path_id, NodeType type) -> Result<NodeId>
{
    // Fast path: node already exists
    auto const* existing = graph.path_to_node.find(to_underlying(path_id));
    if (existing) {
        if (type == NodeType::Generated) {
            auto* node = get_file_node(graph, *existing);
            if (node && (node->type == NodeType::Ghost || node->type == NodeType::File)) {
                node->type = NodeType::Generated;
            }
        }
        return *existing;
    }

    // Recursively ensure parent directory exists
    auto parent_path = graph.paths.parent(path_id);
    auto parent_result = ensure_file_node(graph, parent_path, NodeType::Directory);
    if (!parent_result) {
        return parent_result;
    }

    // Check if node exists by dir+name (may have been created without path_to_node entry)
    auto name = graph.paths.name(path_id);
    if (auto found = find_by_dir_name(graph, *parent_result, global_pool().get(name))) {
        // Update path_to_node for future fast-path hits
        graph.path_to_node.insert(to_underlying(path_id), *found);
        if (type == NodeType::Generated) {
            auto* node = get_file_node(graph, *found);
            if (node && (node->type == NodeType::Ghost || node->type == NodeType::File)) {
                node->type = NodeType::Generated;
            }
        }
        return *found;
    }

    auto node = FileNode {
        .type = type,
        .name = name,
        .parent_dir = *parent_result,
    };
    return add_file_node(graph, std::move(node));
}
```

Note: the `find_by_dir_name` fallback handles nodes created by other code paths (e.g., input resolution) that didn't go through `ensure_file_node`. `add_file_node` already populates `path_to_node`.

- [ ] **Step 5: Build and run tests**

Run: `make && ./build/test/unit/putup_test "[graph]" -s 2>&1 | tail -15`
Expected: All graph tests PASS including the new `ensure_file_node` tests.

- [ ] **Step 6: Commit**

```bash
git add include/pup/graph/dag.hpp src/graph/dag.cpp test/unit/test_graph.cpp
git commit -m "Add ensure_file_node: PathId-native node creation"
```

---

### Task 4: Change `CommandInfo.outputs` to `Vec<PathId>`

**Files:**
- Modify: `include/pup/graph/rule_pattern.hpp:17-25`
- Modify: `test/unit/test_rule_pattern.cpp` (43 instances)
- Modify: `test/unit/test_dep_scanner.cpp` (22 instances)

- [ ] **Step 1: Change `CommandInfo.outputs` type**

In `include/pup/graph/rule_pattern.hpp`, add include at line 7 (after the existing includes):

```cpp
#include "pup/core/path_id.hpp"
```

Change line 23 in the `CommandInfo` struct:

```cpp
    Vec<PathId> outputs;
```

- [ ] **Step 2: Build to find all compilation errors**

Run: `make 2>&1 | grep "error:" | head -20`
Expected: Errors in `test_rule_pattern.cpp`, `test_dep_scanner.cpp`, and `builder.cpp` where `CommandInfo.outputs` is constructed with `Vec<StringId>`.

- [ ] **Step 3: Update test helpers — add a path interning helper**

Both test files need to convert string output paths to `PathId`. The tests use a local `intern()` helper that returns `StringId`. Add a `path()` helper to each test file's anonymous namespace.

In `test/unit/test_rule_pattern.cpp`, add to the anonymous namespace (after the existing `intern` helper):

```cpp
auto path(std::string_view s) -> PathId
{
    // Tests don't use a Graph, so use a file-local PathPool
    static auto paths = PathPool {};
    return paths.intern_path(s, global_pool());
}
```

In `test/unit/test_dep_scanner.cpp`, add the same helper.

- [ ] **Step 4: Update all `CommandInfo` constructions in test_rule_pattern.cpp**

Change every `.outputs = { intern("foo.o") }` to `.outputs = { path("foo.o") }` across all 43 instances.

The pattern is mechanical: replace `intern(` with `path(` inside `.outputs = { ... }` lines. Examples:

```cpp
// Before:
.outputs = { intern("foo.o") },

// After:
.outputs = { path("foo.o") },
```

For multi-output cases:
```cpp
// Before:
.outputs = { intern("foo.o"), intern("bar.o") },

// After:
.outputs = { path("foo.o"), path("bar.o") },
```

- [ ] **Step 5: Update all `CommandInfo` constructions in test_dep_scanner.cpp**

Same mechanical change across all 22 instances.

- [ ] **Step 6: Build and run scanner/pattern tests**

Run: `make && ./build/test/unit/putup_test "[rule_pattern][dep_scanner]" -s 2>&1 | tail -15`
Expected: All rule_pattern and dep_scanner tests PASS. The scanners don't read `cmd.outputs`, so behavior is unchanged.

- [ ] **Step 7: Commit**

```bash
git add include/pup/graph/rule_pattern.hpp test/unit/test_rule_pattern.cpp test/unit/test_dep_scanner.cpp
git commit -m "Change CommandInfo.outputs from Vec<StringId> to Vec<PathId>"
```

---

### Task 5: `build_dep_command` — get source files from `cmd.inputs`

**Files:**
- Modify: `src/graph/scanners/gcc.cpp:248-337`
- Modify: `test/unit/test_rule_pattern.cpp`
- Modify: `test/unit/test_dep_scanner.cpp`

- [ ] **Step 1: Write a failing test — source files from inputs, not command text**

The current `build_dep_command` extracts source files from the command string. After this change, it should extract them from `cmd.inputs`. To prove this, create a test where `cmd.command` uses `%f` placeholder instead of literal source file paths.

Add a new SECTION in the existing "GCC depfile pattern" TEST_CASE in `test/unit/test_rule_pattern.cpp`:

```cpp
    SECTION("matches instruction pattern with %f placeholder")
    {
        auto cmd = CommandInfo {
            .node_id = 900,
            .command = intern("gcc -c -o %o %f"),
            .display = intern("CC %o"),
            .inputs = { intern("src/main.c") },
            .order_only_inputs = {},
            .outputs = { path("main.o") },
            .working_dir = intern("."),
        };

        auto generated = registry.match_and_generate(cmd);
        REQUIRE(generated.size() == 1);
        REQUIRE(generated[0].command == intern("gcc -M src/main.c"));
    }

    SECTION("instruction pattern with multiple source files from inputs")
    {
        auto cmd = CommandInfo {
            .node_id = 901,
            .command = intern("gcc -I include -c -o %o %f"),
            .display = intern("CC %o"),
            .inputs = { intern("a.c"), intern("b.c") },
            .order_only_inputs = {},
            .outputs = { path("out.o") },
            .working_dir = intern("."),
        };

        auto generated = registry.match_and_generate(cmd);
        REQUIRE(generated.size() == 1);
        REQUIRE(generated[0].command == intern("gcc -M -Iinclude a.c b.c"));
    }
```

- [ ] **Step 2: Run test to verify it fails**

Run: `make && ./build/test/unit/putup_test "[rule_pattern]" -s 2>&1 | tail -10`
Expected: FAIL — `matches_gcc_compile` returns false because `%f` has no `-c` when the command has `%f` as a literal token... wait, the command `gcc -c -o %o %f` does have `-c`. So `matches` will pass. But `build_dep_command` won't find source files (because `is_source_file("%f")` is false). The generated dep command will be `gcc -M` with no source files — test should fail on the REQUIRE check.

- [ ] **Step 3: Modify `build_dep_command` to get source files from `cmd.inputs`**

In `src/graph/scanners/gcc.cpp`, modify `GccScanner::build_dep_command` (line 248). Replace the source-file extraction loop and appendage (lines 326-334) with input-based extraction.

Replace lines 291-334 (from `auto skip_next = false;` through the source files loop) with:

```cpp
    auto skip_next = false;
    for (auto i = compiler_idx + 1; i < words.size(); ++i) {
        if (skip_next) {
            dep_cmd += ' ';
            auto norm = Buf {};
            normalize_path_lexically_into(norm, words[i]);
            shell_quote_into(dep_cmd, norm.view());
            skip_next = false;
            continue;
        }

        auto w = words[i];

        if (w == "-c") {
            continue;
        }

        if (w == "-o") {
            ++i;
            continue;
        }

        if (is_dep_relevant_flag(w)) {
            dep_cmd += ' ';
            auto norm = Buf {};
            normalize_flag_path_into(norm, w);
            shell_quote_into(dep_cmd, norm.view());
            if (w == "-I" || w == "-D" || w == "-U" || w == "-include"
                || w == "-isystem" || w == "-iquote" || w == "-isysroot") {
                skip_next = true;
            }
            continue;
        }

        // Skip source files in command text — they come from cmd.inputs now
        // Also skip pattern placeholders like %f, %o
    }

    // Source files from structured inputs (not from command text tokenization)
    for (auto input_id : cmd.inputs) {
        auto input_sv = pool.get(input_id);
        if (is_source_file(input_sv)) {
            dep_cmd += ' ';
            shell_quote_into(dep_cmd, input_sv);
        }
    }
```

- [ ] **Step 4: Build and run tests**

Run: `make && ./build/test/unit/putup_test "[rule_pattern][dep_scanner]" -s 2>&1 | tail -15`
Expected: All tests PASS. Existing tests still work because their `cmd.inputs` already contains the same source files that were in the command text. The new instruction-pattern tests also pass.

- [ ] **Step 5: Run E2E tests to verify no regressions**

Run: `./build/test/unit/putup_test "[e2e]" -s 2>&1 | tail -10`
Expected: All E2E tests PASS.

- [ ] **Step 6: Commit**

```bash
git add src/graph/scanners/gcc.cpp test/unit/test_rule_pattern.cpp
git commit -m "Get source files from cmd.inputs instead of command text tokenization"
```

---

### Task 6: Rewrite `expand_outputs` to return `Vec<PathId>`

**Files:**
- Modify: `src/graph/builder.cpp:1015-1059`

- [ ] **Step 1: Change `expand_outputs` return type and body**

In `src/graph/builder.cpp`, rewrite `expand_outputs` (lines 1015-1059):

```cpp
auto expand_outputs(
    BuilderContext& ctx,
    Vec<parser::PathPattern> const& patterns,
    parser::PatternFlags const& flags
) -> Result<Vec<PathId>>
{
    auto& pool = global_pool();
    auto result = Vec<PathId> {};

    for (auto const& pattern : patterns) {
        if (pattern.is_group) {
            continue;
        }
        if (pattern.is_output_exclusion) {
            continue;
        }

        auto paths = parser::expand_path(*ctx.eval, pattern);
        if (!paths) {
            return pup::unexpected<Error>(paths.error());
        }

        for (auto path_id : *paths) {
            auto expanded = parser::expand_pattern(*ctx.eval, pool.get(path_id), flags);
            auto output_path_sv = expanded ? pool.get(*expanded) : pool.get(path_id);

            auto full_output_path_sv = pool.get(pup::path::normalize(pool.get(pup::path::join(str(ctx.current_dir), output_path_sv))));

            // Intern into PathPool — pure path computation, no graph nodes
            result.push_back(ctx.state->graph.paths.intern_path(full_output_path_sv, pool));
        }
    }

    return result;
}
```

- [ ] **Step 2: Build to find dependent compilation errors**

Run: `make 2>&1 | grep "error:" | head -20`
Expected: Errors in `expand_rule` where `*outputs` is used as `Vec<StringId>` (in `expand_command` calls and the output edge loop). These are fixed in the next tasks.

- [ ] **Step 3: Note — do NOT commit yet**

This step creates compilation errors that are fixed in Tasks 7 and 8. Continue to the next task.

---

### Task 7: Split `expand_command` for instruction vs display paths

**Files:**
- Modify: `src/graph/builder.cpp:930-975, 1919-1952`

- [ ] **Step 1: Create `expand_instruction_pattern` — variable expansion only**

Add a new function before `expand_command` in `src/graph/builder.cpp`:

```cpp
/// Expand a command/display expression through variable expansion only.
/// Returns the result after variable expansion but BEFORE pattern substitution.
/// Used to capture instruction patterns with %o/%f intact.
auto expand_instruction_pattern(
    BuilderContext& ctx,
    parser::Expression const& cmd
) -> Result<StringId>
{
    auto& pool = global_pool();
    auto literal = parser::expand(*ctx.eval, cmd);
    if (!literal) {
        return pup::unexpected<Error>(literal.error());
    }

    auto expanded = parser::expand(*ctx.eval, pool.get(*literal));
    if (!expanded) {
        return pup::unexpected<Error>(expanded.error());
    }

    return *expanded;
}
```

- [ ] **Step 2: Modify `expand_command` to accept `Vec<PathId>`**

Change the existing `expand_command` signature to accept `Vec<PathId>`:

```cpp
auto expand_command(
    BuilderContext& ctx,
    parser::Expression const& cmd,
    parser::PatternFlags flags,
    Vec<PathId> const& outputs,
    StringId* out_instruction = nullptr
) -> Result<StringId>
{
    auto& pool = global_pool();
    auto literal = parser::expand(*ctx.eval, cmd);
    if (!literal) {
        return pup::unexpected<Error>(literal.error());
    }

    auto expanded = parser::expand(*ctx.eval, pool.get(*literal));
    if (!expanded) {
        return pup::unexpected<Error>(expanded.error());
    }

    if (out_instruction) {
        *out_instruction = *expanded;
    }

    auto tc = make_transform_context(ctx);
    auto cmd_outputs = Vec<StringId> {};
    cmd_outputs.reserve(outputs.size());
    for (auto out : outputs) {
        auto path_buf = Buf {};
        ctx.state->graph.paths.write(out, path_buf, pool);
        cmd_outputs.push_back(transform_output_path(tc, path_buf.view()));
    }

    auto primary_output_sv = cmd_outputs.empty() ? std::string_view {} : str(cmd_outputs[0]);
    flags.output = primary_output_sv;
    flags.output_base = parser::path_basename(primary_output_sv);
    auto outputs_sv = Vec<std::string_view> {};
    outputs_sv.reserve(cmd_outputs.size());
    for (auto id : cmd_outputs) {
        outputs_sv.push_back(str(id));
    }
    flags.all_outputs = std::move(outputs_sv);

    auto pattern_result = parser::expand_pattern(*ctx.eval, pool.get(*expanded), flags);
    if (!pattern_result) {
        return pup::unexpected<Error>(pattern_result.error());
    }
    return *pattern_result;
}
```

- [ ] **Step 3: Update `lookup_bang_macro` to use empty `Vec<PathId>`**

In `lookup_bang_macro` (around line 985), change:

```cpp
    auto expanded_cmd = expand_command(ctx, command, flags, {});
```

This already works — `{}` constructs an empty `Vec<PathId>`.

- [ ] **Step 4: Update `expand_rule` to use the split functions**

In `expand_rule` (around lines 1919-1952), the main command expansion should use `expand_instruction_pattern` for the instruction, and `expand_command` only for display text.

Replace the command expansion block (lines 1924-1952) with:

```cpp
    // Capture instruction pattern (variable expansion only, %o/%f intact)
    if (macro_ptr) {
        auto instr = expand_instruction_pattern(ctx, macro_ptr->command);
        if (!instr) {
            return pup::unexpected<Error>(instr.error());
        }
        instruction_pattern = *instr;

        // Expand command with outputs for display and final cmd_text
        auto macro_cmd = expand_command(ctx, macro_ptr->command, flags, *outputs);
        if (!macro_cmd) {
            return pup::unexpected<Error>(macro_cmd.error());
        }
        cmd_text = *macro_cmd;

        if (macro_ptr->display) {
            auto disp_result = expand_command(ctx, *macro_ptr->display, flags, *outputs);
            if (disp_result) {
                display = *disp_result;
            }
        }
    } else {
        auto instr = expand_instruction_pattern(ctx, rule.command);
        if (!instr) {
            return pup::unexpected<Error>(instr.error());
        }
        instruction_pattern = *instr;

        auto full_cmd = expand_command(ctx, rule.command, flags, *outputs);
        if (!full_cmd) {
            return pup::unexpected<Error>(full_cmd.error());
        }
        cmd_text = *full_cmd;

        if (rule.display) {
            auto disp_result = expand_command(ctx, *rule.display, flags, *outputs);
            if (disp_result) {
                display = *disp_result;
            }
        }
    }
```

- [ ] **Step 5: Update `CommandInfo` construction to use instruction pattern**

In `expand_rule` (around line 1979), change the `CommandInfo` construction:

```cpp
    auto cmd_info = CommandInfo {
        .node_id = *cmd_id,
        .command = instruction_pattern,  // instruction pattern, not fully-expanded text
        .display = display,
        .inputs = file_inputs,
        .order_only_inputs = order_only_paths,
        .outputs = *outputs,             // now Vec<PathId>
        .working_dir = intern(str(ctx.current_dir)),
    };
```

Note: `cmd_info.command` is now the instruction pattern (with `%o`/`%f` placeholders) rather than the fully-expanded `cmd_text`. Scanners match against this.

- [ ] **Step 6: Note — do NOT commit yet**

Continue to Task 8 to fix the output edge loop, then build and test everything together.

---

### Task 8: Update output edge loop to use `ensure_file_node`

**Files:**
- Modify: `src/graph/builder.cpp:2018-2103`

- [ ] **Step 1: Replace `get_or_create_file_node` with `ensure_file_node` in the output edge loop**

In `expand_rule` (around lines 2018-2055), replace the output iteration:

```cpp
    // Create edges from command to outputs and collect operand NodeIds
    auto output_ids = Vec<NodeId> {};
    for (auto output_path : *outputs) {
        auto output_id = ensure_file_node(ctx.state->graph, output_path, NodeType::Generated);
        if (!output_id) {
            return pup::unexpected<Error>(output_id.error());
        }

        // Check for duplicate output - another command already produces this file
        auto output_inputs = get_inputs(ctx.state->graph, *output_id);
        if (!output_inputs.empty()) {
            for (auto existing_id : output_inputs) {
                if (node_id::is_command(existing_id)) {
                    auto const* existing_cmd = get_command_node(ctx.state->graph, existing_id);
                    if (existing_cmd && are_guards_mutually_exclusive(existing_cmd->guards, ctx.condition_stack)) {
                        continue;
                    }
                    auto existing_cmd_id = expand_instruction(ctx.state->graph, existing_id, ctx.state->path_cache);
                    auto existing_cmd_sv = global_pool().get(existing_cmd_id);
                    if (existing_cmd_sv.empty()) {
                        existing_cmd_sv = "<unknown>";
                    }
                    auto output_path_sv = get_full_path(ctx.state->graph, *output_id, ctx.state->path_cache);
                    auto err = Buf {};
                    err.fmt("Unable to create output '{}' because it is already owned by command:\n  {}", output_path_sv, existing_cmd_sv);
                    return make_error<void>(ErrorCode::DuplicateNode, err.view());
                }
            }
        }

        auto edge_result = add_edge(ctx.state->graph, *cmd_id, *output_id);
        if (!edge_result) {
            return pup::unexpected<Error>(edge_result.error());
        }
        output_ids.push_back(*output_id);
```

The rest of the loop (group handling, lines 2057-2103) uses `*output_id` which is still a `NodeId` — unchanged.

- [ ] **Step 2: Build**

Run: `make 2>&1 | tail -20`
Expected: Successful compilation. All type errors from Tasks 6-8 should be resolved.

- [ ] **Step 3: Run unit tests**

Run: `./build/test/unit/putup_test -s 2>&1 | tail -15`
Expected: All tests PASS.

- [ ] **Step 4: Run E2E tests**

Run: `./build/test/unit/putup_test "[e2e]" -s 2>&1 | tail -15`
Expected: All E2E tests PASS.

- [ ] **Step 5: Run the full check suite**

Run: `make check 2>&1 | tail -20`
Expected: format-check + tidy + all tests PASS.

- [ ] **Step 6: Commit Tasks 6-8 together**

These three tasks form one atomic change — `expand_outputs` return type, `expand_command` split, and output edge loop update.

```bash
git add src/graph/builder.cpp
git commit -m "Thread PathId through expand_outputs and output edge loop

expand_outputs returns Vec<PathId> instead of Vec<StringId>.
No graph nodes created during output expansion — deferred to
ensure_file_node in the edge creation loop.

expand_command materializes PathId via PathPool::write + Buf
only for display text. Instruction pattern captured separately
with %o/%f placeholders intact.

CommandInfo.command now stores the instruction pattern, not the
fully-expanded command text."
```

---

### Task 9: Add follow-up comment to issue #20

**Files:** None (GitHub API only)

- [ ] **Step 1: Add follow-up items as a comment on the issue**

```bash
gh issue comment 20 --repo typeless/putup --body "$(cat <<'EOF'
## Follow-up items from this change

1. **Defer display text expansion to execution time** — Currently display text (e.g., `^ CC %o ^`) is still materialized at graph-building time via `PathPool::write` + `Buf`. A follow-up should store the display pattern with `%o`/`%f` intact and expand at display time from operand NodeIds, same as instruction patterns.

2. **Thread `PathId` through `CommandInfo.inputs`** — Inputs stay as `Vec<StringId>` because they carry group references (`dir/<groupname>`) that aren't file paths. A future phase should introduce a representation that handles both (possibly tagged union `PathId | GroupRef`).
EOF
)"
```

- [ ] **Step 2: Done**

No commit needed.

---

## Verification Checklist

After all tasks are complete, verify:

- [ ] `make check` passes (format + tidy + tests)
- [ ] `./build/test/unit/putup_test "[e2e]"` — all E2E tests pass
- [ ] `./build/test/unit/putup_test "[path_pool]"` — new `write` tests pass
- [ ] `./build/test/unit/putup_test "[graph]"` — new `ensure_file_node` tests pass
- [ ] `./build/test/unit/putup_test "[rule_pattern]"` — scanner matching works with instruction patterns
- [ ] `./build/test/unit/putup_test "[dep_scanner]"` — dep scanner works with instruction patterns
- [ ] `make iwyu` — no dead includes introduced
