# Pup Code Review Summary

Date: 2025-12-18

## Remaining Issues

### Medium Priority (Performance)

| Area | Issue | Impact |
|------|-------|--------|
| **DAG** | `nodes_of_type()` is O(n) scan | Low impact: called once per build, not per-node |
| **Builder** | `expand_inputs()` is 241 lines | Split into helpers when modifying |
| **Scheduler** | `notify_all()` on every job | ~0.2% overhead, `notify_one()` would serialize parallel jobs |
| **Topo** | Topological sort recomputed each call | N/A: called once per build; node_depth/max_depth are dead code |
| **Parser** | 140-line switch statement | Verbose but O(1); heterogeneous handlers resist dispatch table |
| **Eval** | Three `find()` calls per iteration | O(k×n) but k,n small; single-pass would be O(n) |
| **Hash** | Uses raw `new`/`delete` | Correct code; unique_ptr would reduce boilerplate |

### Low Priority (Tech Debt)

| Area | Issue | Notes |
|------|-------|-------|
| **Platform** | Stdin pipe deadlock with large data | Unused feature (process-posix.cpp:166) |
| **Builder** | `expand_inputs()` complexity | 8+ interleaved cases, well-tested but hard to modify |
| **Testability** | `std::exit()` in error paths | Commands not fully unit-testable |
| **Testability** | Heavy `std::filesystem` coupling | No dependency injection |
| **Paths** | Path normalization scattered | Consolidate utilities |

### Won't Fix

| Area | Issue | Rationale |
|------|-------|-----------|
| **DAG** | Triple edge storage | Intentional: edges_ for serialization, adjacency for O(1) traversal |
| **DAG** | Sparse vector with holes | O(1) lookup, nodes rarely deleted |
| **Index** | No endianness handling | Local artifacts, never shared cross-arch |
| **Builder** | Verbose mode accumulates errors | Intentional: report all Tupfile errors |

---

## Architecture Notes

### CLI Module (`src/cli/`)

Commands extracted from main.cpp (now 59 lines):

| File | Purpose |
|------|---------|
| `cmd_build.cpp` | Build command (835 lines) |
| `cmd_clean.cpp` | Clean/distclean commands |
| `cmd_export.cpp` | Export graph/script/compdb |
| `cmd_init.cpp` | Init command |
| `cmd_parse.cpp` | Parse command |
| `cmd_variant.cpp` | Variant management |
| `context.cpp` | Build context (source/output roots) |
| `multi_variant.cpp` | Multi-variant orchestration |
| `options.cpp` | CLI parsing (`std::from_chars`) |
| `target.cpp` | Target parsing (variant/scope/glob) |

### Builder Complexity (`expand_inputs()`)

Three distinct group lookup patterns with different path normalization:
1. `is_order_only_group` with empty path → uses `current_dir`
2. `is_order_only_group` with non-empty path → uses `normalize_group_dir(expanded_path)`
3. `path/<group>` pattern → uses `normalize_group_dir(dir_part)` always

Previous refactoring attempt caused regression (unit tests passed, real build failed). Well-tested in `test_builder.cpp` (11 test cases).

---

## Strengths

- Clean C++20: `auto`, trailing returns, designated initializers
- Strong types: `NodeId`, `LinkType` prevent bugs
- Exception-free: `Result<T>` with `-fno-exceptions -fno-rtti`
- Module separation: core/parser/graph/index/exec/cli
- Atomic writes: temp + rename pattern
- O(1) index lookup: tagged ID spaces (format v7)
- Test coverage: 1821 assertions in 235 test cases
- Binary size: 1.1MB (15% smaller than with exceptions)
- Unified CLI: path-based variants, globs, single output targets
