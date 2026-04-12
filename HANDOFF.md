# Lazy Input Grounding — Handoff

**Branch:** `wip/lazy-input-grounding`
**Issue:** #21
**Updated:** 2026-04-12

## Where we are

All tasks complete. `ensure_file_node` is now the sole node creation path for both inputs and outputs. Six functions deleted, 253 lines removed.

### PRs

| PR | Branch | Status | Change |
|----|--------|--------|--------|
| #29 | `wip/remove-get-or-create-directory-node` | Merged | Task 6: directory node callers → ensure_file_node (-42/+4) |
| #28 | `wip/resolve-input-via-ensure-file-node` | Merged | Task 2: resolve_input_node internals → ensure_file_node (-119/+15) |
| #27 | `wip/migrate-get-or-create-file-node` | Merged | Task 5: include/Tupfile nodes → ensure_file_node (-92/+4) |
| #28 | `wip/resolve-input-via-ensure-file-node` | Draft, CI pending | Task 2: resolve_input_node internals → ensure_file_node (-119/+15) |

### Deleted functions (cumulative)

- `get_or_create_file_node` (80 lines) — PR #27
- `normalize_path` (7 lines) — PR #27
- `walk_to_file_node` (40 lines) — PR #28
- `walk_path_to_directory` (52 lines) — PR #28

## What we know (verified 2026-04-12)

### The input pipeline has three phases

```
expand_inputs()          →  Vec<StringId>  (mixed: glob patterns + resolved paths)
    ↓
process_rule()           →  foreach? separate patterns/files, loop expand_rule() per file
    ↓
expand_rule()
  ├─ glob/file separation (again — same has_glob_chars filter)
  ├─ transform_input_path()  →  cmd_inputs (Tupfile-relative, for %f/%B/%g)
  ├─ expand_command()        →  instruction text
  ├─ resolve_input_node()    →  NodeId (filesystem probe, type inference, node creation)
  └─ ensure_file_node()      →  NodeId (outputs only, already PathId-based)
```

Any redesign must preserve this structure. `expand_inputs` must keep returning `Vec<StringId>`.

### foreach is purely syntactical

- Lives in exactly two places: `Rule::foreach_` AST flag, and `process_rule()` (builder.cpp:2149–2168)
- Zero references in `dag.cpp`, `src/index/`, or `src/exec/`
- Produces a different graph topology (N commands × 1 input vs 1 command × N inputs) but the graph has no concept of "these came from the same rule"
- The phase boundary is clean: foreach finishes before node resolution begins

### expand_inputs returns mixed content

`expand_inputs` returns **both** raw glob patterns and expanded files in the same `Vec<StringId>`. Example: for input `*.c` with files `add.c`, `main.c`:

```
result = ["*.c", "add.c", "main.c"]
```

The consumer (`process_rule`) separates them:
- `has_glob_chars()` → patterns (kept for `%g` expansion)
- everything else → files (drive foreach iteration or become inputs)

This is intentional, not a bug.

### expand_rule does its own glob/file separation

`expand_rule` always filters its `inputs` parameter through `has_glob_chars`, regardless of whether foreach already separated them. In the foreach case this is mostly redundant; in the non-foreach case this is the **only** separation point. `glob_pattern` takes the last glob (for `%g`); `file_inputs` gets everything else.

### file_inputs is used in four places inside expand_rule

| Use | Line | Purpose |
|-----|------|---------|
| `transform_input_path` | 1656 | → `cmd_inputs` (Tupfile-relative paths for `%f`/`%B`/`%g` pattern expansion) |
| `CommandInfo.inputs` | 1871 | Passed to scanner/pattern registries for dep-scan matching |
| `resolve_input_node` | 1906 | Creates/finds graph nodes, adds input→command edges |
| (via cmd_inputs) | 1683 | `PatternFlags.input` = first cmd_input, drives `%f`/`%B`/`%e` |

### resolve_input_node has 7 branches

Two prefix-stripping transforms at the top (build prefix, `..` normalization), then:

| # | Check | Result |
|---|-------|--------|
| 1 | Existing node under BUILD_ROOT? | Return it |
| 2 | Had build prefix? | Ghost under BUILD_ROOT |
| 3 | Existing node under SOURCE_ROOT? | Return it |
| 4 | File exists in source_root? | File under SOURCE_ROOT |
| 5 | File exists in config_root? (3-tree) | File under SOURCE_ROOT |
| 6 | File exists in output_root? | Ghost under BUILD_ROOT |
| 7 | Default | Ghost under BUILD_ROOT |

**Priority order matters:** BUILD_ROOT is checked before SOURCE_ROOT. A generated file shadows a source file at the same relative path. Deliberate for variant builds.

**Four call sites** (all use result identically as NodeId → add_edge):
- `expand_rule` regular inputs, order-only inputs
- `process_generated_rules` regular inputs, order-only inputs

### Two parallel node lookup mechanisms

After Task 2, `resolve_input_node` uses a hybrid approach:

- **Branches 1, 3:** `find_by_path(graph, path, ROOT_ID)` — string-based walk through directory children. O(depth) per component. Used as early-return optimization to avoid filesystem I/O.
- **Branches 2, 4–7:** `intern_path` + `ensure_file_node` — PathId trie + `path_to_node` hash map. O(log n) via `SortedPairVec`.

**Consistency:** These are guaranteed consistent. Every node created via `add_file_node()` (the sole creation function) immediately populates both `dir_children` (for `find_by_path`) and `path_to_node` (for PathId lookup). Neither can find a node the other can't, for grounded paths.

**Asymmetry:** `path_to_node` can hold *alias* entries for ungrounded PathIds (created by `ensure_file_node`'s lazy grounding at lines 144, 156). These have no string representation and are invisible to `find_by_path`. This is by design — aliases prevent duplicate nodes when the same file is referenced via different PathId groundings.

**`path_to_node` is runtime-only** — not persisted to the index. Rebuilt from scratch each build via `add_file_node` calls during graph construction.

**Six `find_by_path` call sites remain** across three use cases:
- `resolve_input_node` branches 1, 3 (input resolution)
- `transform_input_path` line 234 (path rewriting)
- `collect_affected_commands` lines 1142, 1144 (incremental builds)
- `cmd_build.cpp` line 840 (CLI target resolution)

**Future opportunity:** All six `find_by_path` calls could be replaced by `intern_path` (cheap) + `path_to_node` lookup (O(log n) → O(1) with hash map). This would unify the lookup mechanism. Not a correctness concern — purely performance/architecture.

### transform_input_path is a third source of "where does this file live?" logic

Lines 232–270. A path rewriter (not a node creator) that transforms source-relative input paths into Tupfile-relative paths for command expansion. Has its own mini resolve heuristic:

1. Check BUILD_ROOT graph via `find_by_path` → `get_full_path` → make source-relative
2. Check output_root filesystem → make source-relative
3. Check config_root filesystem → canonical relative path
4. Default: make source-relative from input as-is

**No data dependency with resolve_input_node.** Both consume `file_inputs` independently. The current ordering (transform first, resolve second) is code convention, not a requirement.

**If resolve ran first**, `transform_input_path` could be simplified to `get_full_path(node_id)` + `make_source_relative()`. **Exception:** the config-root branch (lines 257–265) calls `canonical()` to resolve symlinks, which `get_full_path` does not do.

### expand_instruction only needs NodeId + path trie

`expand_instruction` (dag.cpp) reconstructs the final command from `cmd->inputs` / `cmd->outputs` (Vec<NodeId>) via `get_full_path`. Does **not** read NodeType. Refactoring node creation doesn't affect command reconstruction.

### get_or_create_directory_node — two callers remain

Cannot be deleted yet:
- **`get_or_create_group_node`:** Custom Group node creation with angle-bracket names
- **Config var directory:** Just needs a directory NodeId

**Semantic oddity:** Always roots at `SOURCE_ROOT_ID`, even for config var directories that conceptually belong to the build directory.

### What the original subagent got wrong

1. **Phase violation** — returned PathIds from `expand_inputs`, crossing the foreach phase boundary
2. **Flattened type inference** — `NodeType::Ghost` for everything, losing source/generated distinction
3. **Net negative** — added `PathPool::write` calls everywhere, replacing one string form with another

## Plan status

| Task | Status | Notes |
|------|--------|-------|
| 1. ensure_file_node lazy resolution | Done | 7 new tests, all pass |
| 2. resolve_input_node → ensure_file_node | Done (PR #28) | Awaiting CI |
| 3. process_generated_rules | Done | Same resolve_input_node path, came for free |
| 4. Order-only inputs | Done | Same resolve_input_node path, came for free |
| 5. Migrate get_or_create_file_node callers | Done (PR #27) | Merged |
| 6. Cleanup | Future | get_or_create_directory_node callers, find_by_path unification |
| 7. DESIGN.md update | Future | |

## Future work (not blocking)

1. **Unify find_by_path → path_to_node:** Replace 6 `find_by_path` call sites with `intern_path` + `path_to_node` lookup. Performance improvement, not correctness.
2. **Reorder expand_rule pipeline:** Resolve nodes before transforming paths. Would let `transform_input_path` use `get_full_path(NodeId)` instead of re-probing filesystem. Blocked by config-root `canonical()` edge case.
3. **Migrate get_or_create_directory_node callers:** Groups and config vars could use `intern_path` + `ensure_file_node(Directory)`. Address the SOURCE_ROOT semantic question for config vars.

## Related PRs and issues

- PR #28 (pending) — Task 2: resolve_input_node internals → ensure_file_node
- PR #27 (merged) — Task 5: migrate get_or_create_file_node callers
- PR #25 (merged) — 3-tree repro test
- PR #26 (merged) — rooted path algebra (SourceRoot/BuildRoot/Ungrounded)
- PR #22 (merged) — PathId outputs + `%o` fix
- Issue #23 — 3-tree ghost mismatch (output side fixed, input side is this work)
- Issue #24 — rooted path algebra (merged)
- Issue #21 — this issue

## Key files

- `src/graph/builder.cpp` — `expand_inputs`, `resolve_input_node`, `transform_input_path`, `expand_rule`, `process_rule`, `process_generated_rules`
- `src/graph/dag.cpp` — `ensure_file_node`, `find_by_path`, `expand_instruction`, `materialize_path`, `get_full_path`, `collect_affected_commands`
- `include/pup/graph/dag.hpp` — `ensure_file_node`, `materialize_path`, Graph::path_to_node declarations
- `include/pup/graph/rule_pattern.hpp` — `CommandInfo` struct
