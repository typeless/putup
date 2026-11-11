# Thread PathId Through CommandInfo and Scanner Registries

**Issue:** [typeless/putup#20](https://github.com/typeless/putup/issues/20)
**Date:** 2026-04-09
**Status:** Approved

## Problem

`expand_outputs` materializes output paths to `Vec<StringId>` immediately, even though the output nodes already exist in the graph with `PathId` handles. This causes:

1. **Round-trip waste** — `walk_to_file_node` creates a node (with `PathId`), then `get_full_path` walks the parent chain back to a string, then `get_or_create_file_node` re-resolves that string to a node.
2. **Unnecessary `StringPool::intern` calls** — one per output path per rule.
3. **Premature coupling** — `CommandInfo.outputs` stores strings when scanners don't even read the field.

## Design

### Approach: `Vec<PathId>` (Approach A — minimal dependency)

`PathId` requires only `PathPool` (core type, no graph dependency). `NodeId` requires `Graph`. `StringId` requires `StringPool`. Following the minimal dependency principle, `CommandInfo.outputs` becomes `Vec<PathId>`.

### 1. `expand_outputs` — pure path computation

Returns `Vec<PathId>` instead of `Vec<StringId>`. No graph nodes created — only interning into `PathPool`.

```
Before: expand path → walk_to_file_node → get_full_path → intern → StringId
After:  expand path → PathPool::intern_path → PathId
```

Node creation is deferred to the output edge loop where edges are created.

### 2. Scanner matching (unchanged in this phase)

`CommandInfo.command` stores the fully-expanded command text (with `%o`/`%f` already substituted). Scanners tokenize this text to find compiler, flags, and source files. This is the pre-existing behavior — scanner migration to `cmd.inputs` is a follow-up.

- `matches_gcc_compile()` — tokenizes `cmd.command`, looks at compiler name and `-c` flag. Works unchanged.
- `build_dep_command()` — tokenizes `cmd.command` to extract compiler, flags, and source files. Works unchanged.

### 3. `expand_command` changes

`expand_command` accepts `Vec<PathId>` outputs (was `Vec<StringId>`). Output paths are materialized via `materialize_path()` which prepends the build root name for BuildRoot-grounded paths. This ensures `%o` in the command text includes the correct build-tree prefix for 3-tree builds.

The instruction pattern (with raw `%o`/`%f`) is still captured via the `out_instruction` parameter for execution-time expansion by `expand_instruction`.

### 4. `PathPool::write(PathId, Buf&)`

New method that materializes a path into a caller-provided `Buf` without interning into `StringPool`. Used for display text materialization and any other transient string needs.

### 5. `Buf::INLINE_CAP` increase

Increased from 256 to 4096 bytes. Output paths are short individually, but shell commands (compiler flags, multiple inputs/outputs) can be long. 4096 keeps the common case stack-allocated.

### 6. `ensure_file_node` — PathId-native node creation

New function in `dag.cpp` that creates file nodes directly from `PathId`, walking the `PathPool` trie instead of parsing strings:

```cpp
auto ensure_file_node(Graph& graph, PathId path_id, NodeType type) -> Result<NodeId>
```

- **Fast path:** `path_to_node.find(path_id)` returns existing `NodeId`. Handles type upgrade (Ghost/File → Generated).
- **Create path:** Recursively ensures parent directory node exists via `PathPool::parent()`, then creates leaf node with `PathPool::name()` as basename.
- **Recursion terminates** when `path_to_node` contains a hit (the build root, or any previously-created directory).

No string materialization at any point. `PathPool::name()` returns `StringId` directly, which is what `FileNode::name` stores.

### 7. `CommandInfo` struct

```cpp
struct CommandInfo {
    NodeId node_id = INVALID_NODE_ID;
    StringId command = StringId::Empty;    // fully-expanded command text
    StringId display = StringId::Empty;
    Vec<StringId> inputs;                  // stays StringId (phase 2)
    Vec<StringId> order_only_inputs;       // stays StringId
    Vec<PathId> outputs;                   // changed from Vec<StringId>
    StringId working_dir = StringId::Empty;
};
```

### 8. Output edge loop

Replaces `get_or_create_file_node(ctx, str(output))` with `ensure_file_node(graph, path_id)`. No `Buf`, no `str()`, no string round-trip.

## Affected Files

- `src/graph/builder.cpp` — `expand_outputs`, `expand_command`, `expand_rule`, output edge loop
- `src/graph/dag.cpp` — new `ensure_file_node` function
- `include/pup/graph/dag.hpp` — `ensure_file_node` declaration
- `include/pup/graph/rule_pattern.hpp` — `CommandInfo.outputs` type change, add `path_id.hpp` include
- `include/pup/core/path_pool.hpp` — add `write(PathId, Buf&)` method
- `src/core/path_pool.cpp` — implement `write`
- `include/pup/core/buf.hpp` — increase `INLINE_CAP` to 4096
- `src/graph/scanners/gcc.cpp` — `build_dep_command` gets source files from `cmd.inputs`
- `src/graph/rule_pattern.cpp` — `make_gcc_depfile_pattern` generate closure (no change expected, doesn't read outputs)
- `test/unit/test_rule_pattern.cpp` — update `CommandInfo` construction in tests
- `test/unit/test_dep_scanner.cpp` — update `CommandInfo` construction in tests

## Follow-Up Items

1. **Defer display text expansion to execution time** — Store display pattern with `%o`/`%f` intact, expand at display time from operand NodeIds (same as instruction). Eliminates the last graph-building-time materialization.
2. **Thread `PathId` through `CommandInfo.inputs`** — Inputs carry group references (`dir/<groupname>`) that aren't file paths. Needs a different representation (possibly tagged union `PathId | GroupRef`). Separate issue.
3. **Thread `cmd.inputs` into scanner `build_dep_command`** — Currently tokenizes the expanded command text to find source files. Should read from `cmd.inputs` directly (source-root-relative paths). Blocked on #21 (inputs path duality).

## Benefit

- Eliminates ~N unnecessary `StringPool::intern` calls per build (one per output path)
- Eliminates redundant node lookups (no `get_or_create_file_node` re-resolution)
- Scanners only materialize paths they actually need (currently none read outputs)
- `ensure_file_node` is O(log n) via `path_to_node` for existing nodes — no string parsing
- Follows the algebraic principle: stay in the cheapest type (`PathId`) as long as possible
