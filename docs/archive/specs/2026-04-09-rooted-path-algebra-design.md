# Rooted Path Algebra

**Date:** 2026-04-09
**Status:** Approved

## Problem

Paths in the build graph carry no information about which root they belong to (source tree vs build tree). The interpretation is implicit — scattered across `get_or_create_file_node`, `resolve_input_node`, `strip_path_prefix`, `normalize_to_output_relative`, `transform_input_path`, and `compute_tup_variantdir`. This causes:

1. Ghost/output path mismatches in 3-tree builds (issue #23)
2. `CommandInfo.inputs` duality — source-root-relative for graph, directory-relative for shell (issue #21)
3. `ensure_file_node` Root guard ambiguity
4. `set_build_root_name` hack using string prefix to distinguish domains
5. `get_or_create_file_node` with 5 normalization branches guessing path form

## Design

### Reserved PathIds

```
PathId(0) = Ungrounded    — path fragment with no root (sentinel)
PathId(1) = SourceRoot    — source tree root
PathId(2) = BuildRoot     — build tree root
```

Every grounded PathId chains back to SourceRoot or BuildRoot. Ungrounded PathIds are intermediate forms used during parsing/expansion.

### Operations

#### `intern(parent: PathId, name: StringId) → PathId`

Join one component. Root inherited from parent.

- `intern(Ungrounded, "gcc")` → `Ungrounded→gcc`
- `intern(BuildRoot, "gcc")` → `BuildRoot→gcc`
- `intern(BuildRoot→gcc, "foo.o")` → `BuildRoot→gcc→foo.o`

#### `intern_path(string, pool) → PathId`

Parse slash-separated string. Always starts from Ungrounded.

- `intern_path("gcc/foo.c")` → `Ungrounded→gcc→foo.c`

#### `intern_path(string, pool, root) → PathId`

Parse slash-separated string starting from a specific root.

- `intern_path("gcc/foo.o", pool, BuildRoot)` → `BuildRoot→gcc→foo.o`

#### `parent(id) → PathId`

Decompose. Roots are fixed points: `parent(SourceRoot) = SourceRoot`.

#### `name(id) → StringId`

Decompose. Roots have empty name: `name(BuildRoot) = Empty`.

#### `root(id) → PathId`

Walk to root. Returns one of {Ungrounded, SourceRoot, BuildRoot}.

#### `ground(id, root) → PathId`

Re-intern an ungrounded chain under a root.

- `ground(Ungrounded→gcc→foo.c, SourceRoot)` → `SourceRoot→gcc→foo.c`
- `ground(Ungrounded→gcc→foo.o, BuildRoot)` → `BuildRoot→gcc→foo.o`
- `ground(SourceRoot→gcc→foo.c, BuildRoot)` → **error** (already grounded to different root)
- `ground(BuildRoot→gcc→foo.o, BuildRoot)` → identity (already correct root)

#### `write(id, buf, pool)`

Materialize to string, skipping the root component.

- `write(BuildRoot→gcc→foo.o)` → `"gcc/foo.o"`
- `write(Ungrounded→gcc→foo.o)` → `"gcc/foo.o"`

The root is metadata, not part of the path string.

### Closure properties

| Operation | Root of input | Root of output |
|-----------|--------------|----------------|
| intern(parent, name) | any | same as parent |
| intern_path(string) | N/A | Ungrounded |
| intern_path(string, root) | N/A | root |
| parent(id) | any | same as id (roots are fixed points) |
| ground(id, root) | Ungrounded only | root |
| write(id) | any | N/A (materialization) |

### Interaction rules

**Grounded + ungrounded join:**

`ground_join(grounded_parent, ungrounded_child)` — the parent's root propagates.

```
ground_join(BuildRoot→gcc→libcody, Ungrounded→libcody.a)
  = intern(BuildRoot→gcc→libcody, name(Ungrounded→libcody.a))
  = BuildRoot→gcc→libcody→libcody.a
```

**Cross-domain comparison:**

`SourceRoot→gcc→foo.c ≠ BuildRoot→gcc→foo.c` — different roots, different PathIds. Source files and generated files at the same relative location are structurally distinct.

**`path_to_node` mapping:**

Since SourceRoot→gcc→foo.c and BuildRoot→gcc→foo.c are different PathIds, they map to different NodeIds. No `had_build_prefix` heuristic needed.

### Pipeline path forms

```
Tupfile text         →  strings (uninterned)
Parser/eval          →  StringId (interned but unstructured)
expand_path          →  Ungrounded PathId (structured, no root)
expand_outputs       →  BuildRoot PathId (grounded)
expand_inputs        →  SourceRoot or BuildRoot PathId (grounded)
CommandInfo.outputs  →  BuildRoot PathId
CommandInfo.inputs   →  SourceRoot|BuildRoot PathId
resolve_node         →  NodeId (via path_to_node lookup on grounded PathId)
write/materialize    →  string at system boundary (shell commands, display)
```

### What goes away

- `set_build_root_name` — no string prefix for build root; domain is structural
- `get_or_create_file_node` — replaced by `resolve_node(graph, grounded_path_id)`
- `strip_path_prefix` heuristics — domain tag replaces prefix detection
- `had_build_prefix` flag — `root(path_id)` determines domain
- `normalize_to_output_relative` — domain tag handles this
- `transform_input_path` / `transform_output_path` — become `to_dir_relative(path, dir)` at system boundary

### Implementation notes

- `PathPool` reserves entries 0-2 at construction (Ungrounded, SourceRoot, BuildRoot)
- `ground()` walks the ungrounded chain, re-interns each component under the target root — O(depth), typically < 10
- `root()` walks parent chain to find the root — cacheable if needed
- `BUILD_ROOT_ID` in the graph maps to `BuildRoot` PathId; `SOURCE_ROOT_ID` maps to `SourceRoot` PathId
- No changes to `StringPool`, `StringId`, or the graph's edge structure

## Relationship to other issues

- **#20** (thread PathId through CommandInfo) — current PR uses unrooted PathId; this design supersedes the approach
- **#21** (separate CommandInfo.inputs paths) — dissolved by grounded PathId carrying domain
- **#23** (3-tree ghost mismatch) — dissolved by structural domain separation
