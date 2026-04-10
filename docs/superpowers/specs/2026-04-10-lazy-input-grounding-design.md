# Lazy Input Grounding

**Issue:** [typeless/putup#21](https://github.com/typeless/putup/issues/21)
**Date:** 2026-04-10
**Status:** Draft

## Problem

Five functions create file nodes in the graph. The input side uses string-prefix heuristics (`strip_path_prefix`, `had_build_prefix`, `normalize_to_output_relative`, filesystem probing) to answer a single binary question: SourceRoot or BuildRoot? This complexity caused the 3-tree ghost mismatch (#23) and makes the codebase hard to reason about.

## Design

### Principle: defer grounding, resolve lazily

Input paths are interned as Ungrounded PathIds. The domain (SourceRoot vs BuildRoot) is resolved at the last moment it's needed — inside `ensure_file_node` — by trying both grounded forms against `path_to_node`.

### `ensure_file_node` becomes the resolver

```
ensure_file_node(graph, path_id, type):
    if is_root(path_id): dispatch by root type

    // Fast path: already grounded
    if path_to_node has path_id: return node (upgrade type)

    // Lazy resolution: try both roots
    build = ground(path_id, BuildRoot)
    if path_to_node has build: return node

    source = ground(path_id, SourceRoot)
    if path_to_node has source: return node

    // Neither exists — create under the appropriate root
    // Type and filesystem probe determine domain
    ...create node...
```

### `expand_inputs` returns `Vec<PathId>`

Currently returns `Vec<StringId>`. Change to `Vec<PathId>`, interned under Ungrounded via `intern_path(normalized_path, pool)`. No grounding at this stage — the domain is not yet needed.

Group references (`<group>`, `{group}`) remain as StringId placeholders — they are not file paths.

### `resolve_input_node` is absorbed into `ensure_file_node`

The heuristic chain (`strip_path_prefix` → `normalize_to_output_relative` → filesystem fallback) is replaced by `ensure_file_node`'s lazy resolution. The binary choice (SourceRoot vs BuildRoot) is determined by:

1. `path_to_node` lookup under BuildRoot — if a producer already created the output, it's there
2. `path_to_node` lookup under SourceRoot — if a source file was already registered
3. Filesystem probe — does the file exist in source tree or build tree?
4. Type tiebreaker — Ghost/Generated → BuildRoot, File → SourceRoot

### What goes away

- `resolve_input_node` — absorbed into `ensure_file_node`
- `get_or_create_file_node` — replaced by `ensure_file_node`
- `strip_path_prefix` — domain is structural, not prefix-based
- `had_build_prefix` flag — `root(path_id)` or `path_to_node` lookup determines domain
- `normalize_to_output_relative` — unnecessary when paths are PathIds

### What changes

- `expand_inputs` returns `Vec<PathId>` (Ungrounded)
- `CommandInfo.inputs` becomes `Vec<PathId>`
- `transform_input_path` takes PathId instead of string
- Caller loop in `expand_rule` calls `ensure_file_node(graph, input_path_id, ...)` directly

### Path lifecycle

```
Tupfile text  →  parser Expression (unresolved variables)
expand_path   →  StringId (variable-expanded, whitespace-split)
expand_inputs →  Ungrounded PathId (current_dir joined, normalized)
ensure_file_node → NodeId (lazily grounded to SourceRoot or BuildRoot)
materialize_path → string (at shell command boundary only)
```

### Cost model

- `ground()` is O(depth), typically < 10 components, and re-interns into existing trie nodes (dedup)
- Two `path_to_node` lookups per ungrounded input (BuildRoot then SourceRoot) — O(1) each
- Filesystem probe only for nodes not in `path_to_node` — same as current behavior

## Open questions (to be resolved during implementation)

1. **Filesystem fallback in `ensure_file_node`**: When neither grounded form is in `path_to_node`, we need to check the filesystem. Should `ensure_file_node` accept source_root/output_root as parameters, or should this be a wrapper function?
2. **Config root overlay**: Tupfiles and config files live under config root but are stored as SourceRoot. The current `get_or_create_file_node` handles this — `ensure_file_node` needs the same.
3. **`transform_input_path`**: Currently takes a string and does graph lookup + `get_full_path`. With PathId inputs, it could take PathId and call `materialize_path` directly. But it also has config-root filesystem fallbacks.

## Dissolves

- #23 — 3-tree ghost/output mismatch (output side fixed in #22, input side fixed here)
- #21 — CommandInfo.inputs path duality (grounded PathId carries domain)
