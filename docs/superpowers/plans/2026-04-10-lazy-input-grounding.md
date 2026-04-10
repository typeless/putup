# Lazy Input Grounding Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace `resolve_input_node` and `get_or_create_file_node` with lazy grounding in `ensure_file_node`, eliminating string-prefix heuristics.

**Architecture:** Input paths interned as Ungrounded PathIds in `expand_inputs`. Domain (SourceRoot vs BuildRoot) resolved lazily by `ensure_file_node` trying both grounded forms against `path_to_node`. Filesystem probe only when neither grounded form exists.

**Tech Stack:** C++17, Catch2 BDD tests, putup PathPool/PathId

---

### Task 1: Teach `ensure_file_node` to resolve ungrounded PathIds

The core change. Currently `ensure_file_node` assumes the caller already grounded the PathId. Add lazy resolution: try BuildRoot first, then SourceRoot, then filesystem probe.

**Files:**
- Modify: `src/graph/dag.cpp:120-163`
- Modify: `include/pup/graph/dag.hpp:149` (add overload with filesystem context)
- Test: `test/unit/test_graph.cpp`

- [ ] **Step 1: Write failing test — ungrounded PathId resolves to existing BuildRoot node**

In `test/unit/test_graph.cpp`, add to the `ensure_file_node` test case:

```cpp
SECTION("resolves ungrounded PathId to existing BuildRoot node")
{
    // First create a node under BuildRoot (as expand_outputs would)
    auto build_path = graph.paths.intern_path("src/lib.a", pool, pup::PathId::BuildRoot);
    auto existing = ensure_file_node(graph, build_path, NodeType::Generated);
    REQUIRE(existing.has_value());

    // Now resolve the same path but ungrounded (as expand_inputs would)
    auto ungrounded = graph.paths.intern_path("src/lib.a", pool);
    REQUIRE(graph.paths.root(ungrounded) == pup::PathId::Ungrounded);
    auto resolved = ensure_file_node(graph, ungrounded, NodeType::Ghost);
    REQUIRE(resolved.has_value());
    REQUIRE(*resolved == *existing);
    // Type should NOT downgrade from Generated to Ghost
    REQUIRE(get_file_node(graph, *resolved)->type == NodeType::Generated);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `./build/test/unit/putup_test "ensure_file_node" -s`
Expected: FAIL — ungrounded PathId creates a separate node under BUILD_ROOT_ID instead of finding the existing BuildRoot node.

- [ ] **Step 3: Implement lazy resolution in `ensure_file_node`**

In `src/graph/dag.cpp`, replace the current `ensure_file_node` (lines 120-163):

```cpp
auto ensure_file_node(Graph& graph, PathId path_id, NodeType type) -> Result<NodeId>
{
    // Root sentinels terminate recursion.
    if (is_root(path_id)) {
        return path_id == PathId::SourceRoot ? SOURCE_ROOT_ID : BUILD_ROOT_ID;
    }

    // Fast path: exact PathId already in path_to_node
    auto const* existing = graph.path_to_node.find(to_underlying(path_id));
    if (existing) {
        auto* node = get_file_node(graph, *existing);
        if (node && type == NodeType::Generated
            && (node->type == NodeType::Ghost || node->type == NodeType::File)) {
            node->type = NodeType::Generated;
        }
        return *existing;
    }

    // Lazy resolution: if ungrounded, try both grounded forms
    if (graph.paths.root(path_id) == PathId::Ungrounded) {
        auto build_id = graph.paths.ground(path_id, PathId::BuildRoot);
        auto const* build_hit = graph.path_to_node.find(to_underlying(build_id));
        if (build_hit) {
            auto* node = get_file_node(graph, *build_hit);
            if (node && type == NodeType::Generated
                && (node->type == NodeType::Ghost || node->type == NodeType::File)) {
                node->type = NodeType::Generated;
            }
            return *build_hit;
        }

        auto source_id = graph.paths.ground(path_id, PathId::SourceRoot);
        auto const* source_hit = graph.path_to_node.find(to_underlying(source_id));
        if (source_hit) {
            return *source_hit;
        }
    }

    // No existing node — recurse to ensure parent, then create
    auto parent_path = graph.paths.parent(path_id);
    auto parent_result = ensure_file_node(graph, parent_path, NodeType::Directory);
    if (!parent_result) {
        return parent_result;
    }

    auto name = graph.paths.name(path_id);
    if (auto found = find_by_dir_name(graph, *parent_result, global_pool().get(name))) {
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

- [ ] **Step 4: Run test to verify it passes**

Run: `./build/test/unit/putup_test "ensure_file_node" -s`
Expected: PASS

- [ ] **Step 5: Write test — ungrounded resolves to existing SourceRoot node**

```cpp
SECTION("resolves ungrounded PathId to existing SourceRoot node")
{
    auto source_path = graph.paths.intern_path("main.c", pool, pup::PathId::SourceRoot);
    auto source_node = walk_to_file_node(graph, SOURCE_ROOT_ID, "main.c", NodeType::File);
    REQUIRE(source_node.has_value());

    auto ungrounded = graph.paths.intern_path("main.c", pool);
    auto resolved = ensure_file_node(graph, ungrounded, NodeType::File);
    REQUIRE(resolved.has_value());
    REQUIRE(*resolved == *source_node);
}
```

- [ ] **Step 6: Run test to verify it passes**

Run: `./build/test/unit/putup_test "ensure_file_node" -s`
Expected: PASS (the BuildRoot miss falls through to SourceRoot)

- [ ] **Step 7: Write test — ungrounded with no existing node creates under BuildRoot for Ghost type**

```cpp
SECTION("ungrounded Ghost creates under BuildRoot when no node exists")
{
    auto ungrounded = graph.paths.intern_path("generated.o", pool);
    auto result = ensure_file_node(graph, ungrounded, NodeType::Ghost);
    REQUIRE(result.has_value());

    auto const* node = get_file_node(graph, *result);
    REQUIRE(node != nullptr);
    REQUIRE(node->type == NodeType::Ghost);
    REQUIRE(graph.paths.root(node->path_id) == pup::PathId::BuildRoot);
}
```

- [ ] **Step 8: Run test, verify it fails, then fix**

The create path still uses the ungrounded parent chain, so the node ends up with an Ungrounded path_id. Fix: when creating a new node from an ungrounded PathId, ground it first. In the create section of `ensure_file_node`, before recursing to parent:

```cpp
    // Ground ungrounded paths before creation — Ghost/Generated → BuildRoot, File → SourceRoot
    if (graph.paths.root(path_id) == PathId::Ungrounded) {
        auto target_root = (type == NodeType::File) ? PathId::SourceRoot : PathId::BuildRoot;
        return ensure_file_node(graph, graph.paths.ground(path_id, target_root), type);
    }
```

Insert this block after the lazy resolution section (after the SourceRoot check) and before the parent recursion. This re-enters `ensure_file_node` with a grounded PathId, which then follows the normal create path.

- [ ] **Step 9: Run all ensure_file_node tests**

Run: `./build/test/unit/putup_test "ensure_file_node" -s`
Expected: All PASS

- [ ] **Step 10: Commit**

```
git add src/graph/dag.cpp test/unit/test_graph.cpp
git commit -m "Teach ensure_file_node to resolve ungrounded PathIds lazily"
```

---

### Task 2: Change `expand_inputs` to return `Vec<PathId>`

Convert the return type and intern paths as Ungrounded PathIds. Group references stay as StringId (handled separately in the caller loop).

**Files:**
- Modify: `src/graph/builder.cpp:1049-1140` (`expand_inputs`)
- Modify: `src/graph/builder.cpp:1732-1760` (caller in `expand_rule` — split file PathIds from group StringIds)
- Modify: `include/pup/graph/rule_pattern.hpp:22` (`CommandInfo.inputs` type)

- [ ] **Step 1: Change `expand_inputs` return type to `Vec<PathId>`**

Change the signature at line 1049:

```cpp
auto expand_inputs(
    BuilderContext& ctx,
    Vec<parser::PathPattern> const& patterns
) -> Result<Vec<PathId>>
```

For regular file patterns (line 1119-1122), change:

```cpp
// Before:
if (!is_empty(ctx.current_dir)) {
    result.push_back(pup::path::normalize(pool.get(pup::path::join(str(ctx.current_dir), path_sv))));
} else {
    result.push_back(path_id);
}

// After:
auto normalized = !is_empty(ctx.current_dir)
    ? pool.get(pup::path::normalize(pool.get(pup::path::join(str(ctx.current_dir), path_sv))))
    : pool.get(path_id);
result.push_back(ctx.state->graph.paths.intern_path(normalized, pool));
```

For group member inputs (line 1067-1069), intern the `get_full_path` result:

```cpp
auto path_sv = get_full_path(ctx.state->graph, id, ctx.state->path_cache);
if (!path_sv.empty()) {
    result.push_back(ctx.state->graph.paths.intern_path(path_sv, pool));
}
```

Order-only group references (`<group>`) and glob patterns don't produce PathIds — they'll need separate handling in the caller. For now, skip them (they go through a different code path in expand_rule).

- [ ] **Step 2: Update the caller loop in `expand_rule`**

The caller at lines 1743-1757 currently iterates `file_inputs` (Vec<StringId>) and calls `transform_input_path` for command expansion and `resolve_input_node` for graph edges.

Change `file_inputs` to `Vec<PathId>`. The loop at lines 1991-2004 becomes:

```cpp
auto input_ids = Vec<NodeId> {};
for (auto input_path : file_inputs) {
    auto input_id = ensure_file_node(ctx.state->graph, input_path, NodeType::Ghost);
    if (!input_id) {
        return pup::unexpected<Error>(input_id.error());
    }
    auto edge_result = add_edge(ctx.state->graph, *input_id, *cmd_id);
    if (!edge_result) {
        return pup::unexpected<Error>(edge_result.error());
    }
    input_ids.push_back(*input_id);
}
```

No `resolve_input_node`. No `is_order_only_group_reference` check (PathIds are never group references).

- [ ] **Step 3: Update `CommandInfo.inputs` to `Vec<PathId>`**

In `include/pup/graph/rule_pattern.hpp` line 22:

```cpp
Vec<PathId> inputs;                   // Ungrounded PathIds (lazily resolved)
```

Update `CommandInfo` construction at line 1971 to use the PathId vec.

- [ ] **Step 4: Update `transform_input_path` to accept PathId**

Change signature to take PathId, use `materialize_path` internally:

```cpp
auto transform_input_path(
    Graph const& graph,
    PathTransformContext const& tc,
    PathId input_path
) -> StringId
{
    auto full_path_sv = materialize_path(graph, input_path);
    auto path_sv = global_pool().get(full_path_sv);
    if (!is_empty(tc.canonical_cwd) && path_sv.starts_with("..")) {
        return make_canonical_relative(tc, path_sv);
    }
    return pup::make_source_relative(path_sv, str(tc.source_to_root), str(tc.current_dir_id));
}
```

Wait — for ungrounded inputs that haven't been resolved yet, `materialize_path` won't prepend the build prefix. The node might not exist yet either. This needs the node to exist first (created by `ensure_file_node` in the edge loop). So `transform_input_path` should be called AFTER the edge loop, or should accept the NodeId and use `get_full_path`.

This is a design tension — `transform_input_path` is called at line 1756 (before the edge loop at 1991) for `%f` substitution in `expand_command`. But we need the node to exist for `get_full_path` to work.

**Resolution**: Keep `transform_input_path` as-is for now (string-based). The `%f` expansion at parse time (in `expand_command`) uses `file_inputs` for pattern flags, not for graph node creation. The execution-time `%f` expansion in `expand_instruction` already uses `get_full_path` on the operand NodeIds. This is the same split as the output side: parse-time `%o` uses `materialize_path`, execution-time `%o` uses `get_full_path`.

So: `transform_input_path` stays string-based, operating on the materialized form. We materialize the PathId for command expansion, but the graph edge uses `ensure_file_node` directly.

- [ ] **Step 5: Fix compilation and run tests**

Run: `make -j$(nproc) && ./build/test/unit/putup_test`
Expected: Full suite passes. Fix any type mismatches from the `Vec<StringId>` → `Vec<PathId>` change.

- [ ] **Step 6: Commit**

```
git add src/graph/builder.cpp include/pup/graph/rule_pattern.hpp
git commit -m "Change expand_inputs to return Vec<PathId>, wire into ensure_file_node"
```

---

### Task 3: Handle `process_generated_rules` callers

Generated rules (`DEP` commands from scanners) produce inputs as `Vec<StringId>` (from command text tokenization). These need to be interned as PathIds before calling `ensure_file_node`.

**Files:**
- Modify: `src/graph/builder.cpp:1144-1205` (`process_generated_rules`)

- [ ] **Step 1: Replace `resolve_input_node` calls in `process_generated_rules`**

At line 1161, change:

```cpp
// Before:
auto input_id = resolve_input_node(ctx, pool.get(input_id_val));

// After:
auto input_path = ctx.state->graph.paths.intern_path(pool.get(input_id_val), pool);
auto input_id = ensure_file_node(ctx.state->graph, input_path, NodeType::Ghost);
```

At line 1185, change:

```cpp
// Before:
auto oi_node = resolve_input_node(ctx, oi);

// After:
auto oi_path = ctx.state->graph.paths.intern_path(oi, pool);
auto oi_node = ensure_file_node(ctx.state->graph, oi_path, NodeType::Ghost);
```

- [ ] **Step 2: Run tests**

Run: `make -j$(nproc) && ./build/test/unit/putup_test`
Expected: All pass.

- [ ] **Step 3: Commit**

```
git add src/graph/builder.cpp
git commit -m "Replace resolve_input_node in process_generated_rules with ensure_file_node"
```

---

### Task 4: Handle order-only inputs in `expand_rule`

Order-only file inputs (non-group, non-glob) at lines 2101-2110 also call `resolve_input_node`. Replace with `ensure_file_node`.

**Files:**
- Modify: `src/graph/builder.cpp:2100-2110`

- [ ] **Step 1: Replace `resolve_input_node` for order-only inputs**

At lines 2101-2110, change:

```cpp
// Before:
for (auto oi : order_only_paths) {
    auto oi_sv = str(oi);
    if (is_order_only_group_reference(oi_sv) || parser::has_glob_chars(oi_sv)) {
        continue;
    }
    auto oi_node = resolve_input_node(ctx, oi_sv);
    (void)add_order_only_edge(ctx.state->graph, *oi_node, *cmd_id);
}

// After:
for (auto oi : order_only_paths) {
    auto oi_sv = str(oi);
    if (is_order_only_group_reference(oi_sv) || parser::has_glob_chars(oi_sv)) {
        continue;
    }
    auto oi_path = ctx.state->graph.paths.intern_path(oi_sv, pool);
    auto oi_node = ensure_file_node(ctx.state->graph, oi_path, NodeType::Ghost);
    if (oi_node) {
        (void)add_order_only_edge(ctx.state->graph, *oi_node, *cmd_id);
    }
}
```

- [ ] **Step 2: Run tests**

Run: `make -j$(nproc) && ./build/test/unit/putup_test`
Expected: All pass.

- [ ] **Step 3: Commit**

```
git add src/graph/builder.cpp
git commit -m "Replace resolve_input_node for order-only inputs with ensure_file_node"
```

---

### Task 5: Migrate remaining `get_or_create_file_node` callers

Two callers remain: include files (line 1295) and Tupfile nodes (line 2288). Both are source-tree files with known domain — intern under SourceRoot.

**Files:**
- Modify: `src/graph/builder.cpp:1295` (include files)
- Modify: `src/graph/builder.cpp:2288` (Tupfile nodes)

- [ ] **Step 1: Replace include file caller**

At line 1295:

```cpp
// Before:
auto inc_node_result = get_or_create_file_node(ctx, inc_rel, NodeType::File);

// After:
auto inc_path = ctx.state->graph.paths.intern_path(inc_rel, pool, PathId::SourceRoot);
auto inc_node_result = ensure_file_node(ctx.state->graph, inc_path, NodeType::File);
```

- [ ] **Step 2: Replace Tupfile node caller**

At line 2288:

```cpp
// Before:
auto tupfile_node_result = get_or_create_file_node(ctx, tupfile_rel, NodeType::File);

// After:
auto& pool = global_pool();
auto tupfile_path = ctx.state->graph.paths.intern_path(tupfile_rel, pool, PathId::SourceRoot);
auto tupfile_node_result = ensure_file_node(ctx.state->graph, tupfile_path, NodeType::File);
```

- [ ] **Step 3: Run tests**

Run: `make -j$(nproc) && ./build/test/unit/putup_test`
Expected: All pass.

- [ ] **Step 4: Commit**

```
git add src/graph/builder.cpp
git commit -m "Migrate include file and Tupfile node creation to ensure_file_node"
```

---

### Task 6: Remove dead code

With all callers migrated, remove `resolve_input_node`, `get_or_create_file_node`, and associated helpers.

**Files:**
- Modify: `src/graph/builder.cpp` (remove functions)
- Modify: `src/core/path_utils.cpp` (remove `strip_path_prefix` if unused)
- Modify: `include/pup/core/path_utils.hpp` (remove declaration)

- [ ] **Step 1: Delete `resolve_input_node`**

Remove lines 555-634 from `src/graph/builder.cpp`.

- [ ] **Step 2: Delete `get_or_create_file_node`**

Remove lines 473-553 from `src/graph/builder.cpp`.

- [ ] **Step 3: Delete `normalize_to_output_relative`**

Remove lines 145-155 from `src/graph/builder.cpp`. Also remove `resolve_under_root` from `src/core/path_utils.cpp` and `include/pup/core/path_utils.hpp` if no other callers remain.

- [ ] **Step 4: Check if `strip_path_prefix` has remaining callers**

Run: `grep -rn 'strip_path_prefix' src/ include/`

If only used in `dag.cpp:find_by_path` and `dag.cpp:collect_affected_commands` (the already-updated rooted path code), keep it. If no callers remain, remove.

- [ ] **Step 5: Delete `get_or_create_directory_node`**

Check if it's still called (it was only used by `get_or_create_file_node`). If dead, remove.

- [ ] **Step 6: Build and run full test suite**

Run: `make -j$(nproc) && ./build/test/unit/putup_test`
Expected: All pass. Verify no linker errors from missing symbols.

- [ ] **Step 7: Commit**

```
git add src/graph/builder.cpp src/core/path_utils.cpp include/pup/core/path_utils.hpp
git commit -m "Remove resolve_input_node, get_or_create_file_node, and associated heuristics"
```

---

### Task 7: Update DESIGN.md and run final verification

**Files:**
- Modify: `DESIGN.md`

- [ ] **Step 1: Update DESIGN.md**

Update the `CommandInfo` struct to show `Vec<PathId> inputs`. Update the "Path resolution" section to describe `ensure_file_node` lazy grounding instead of the heuristic chain.

- [ ] **Step 2: Run full CI-equivalent checks**

```bash
make test      # Full test suite
make format    # clang-format
make tidy      # clang-tidy
```

- [ ] **Step 3: Commit**

```
git add DESIGN.md
git commit -m "Update DESIGN.md: CommandInfo.inputs is Vec<PathId>, lazy grounding model"
```
