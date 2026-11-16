# Tiled data stacks, step 1: encoding decisions

Status: decision record for migration step 1 of [tiled-data-stacks.md](tiled-data-stacks.md). Written 2026-11-16. It transcribes the surface-compare verdict of that day (trigger (a): new types and public API; candidates C and (iii) are the verdict's own), the design-attorney's round 10, and the rulings the verdict left to the implementer, each marked as such. Code: `include/tiled/`, `src/tiled/`, `test/unit/test_tiled_*.cpp`.

## E1 — Pool metadata encoding

Domain: a 2^pool_log2 pool, leaves of 2^min_log2, one metadata byte per tree node, 2^(d+1) bytes with d = pool_log2 − min_log2 and slot 0 unused. Zero must mean "all free", because the production array sits in zero-initialized static storage and is never initialized at run time.

| Candidate | Byte holds | Verdict |
|---|---|---|
| A | node order − largest free order in the subtree (0 = wholly free); one `FULL` value for "nothing free" | rejected: a taken node and a node with both halves taken separately read the same, so `release(offset, size_log2)` with a size one order too large passes a one-byte check and frees two live blocks |
| B | per-node state {Free = 0, Split, Taken} | rejected: `acquire` is a search that must retry the right subtree at every Split node above the target level, so its cases grow with depth; exhaustion is known only after the whole search fails |
| C | A, plus a distinct `TAKEN` (255) beside the derived `FULL` (254) | **chosen**: `acquire` descends once and reads exhaustion from the root; `release` checks liveness with one read; its residual is a constant set of combine boundaries |

Invariant all three share and the type cannot hold: bytes under a taken node stay zero, so the metadata is a function of the taken set and releasing every block returns every byte to zero. Tested by the model-based property in `test_tiled_pool.cpp`.

Descent rule (implementer's ruling; the verdict called the tie-break a policy its model does not determine): take the child whose largest free block is smaller, left on a tie. It is not true best fit: one byte per node cannot see an exact-fit block under a larger free block. Left on a tie puts the first acquire at offset 0, which the heap tile relies on (tiled-data-stacks.md, Layout and realization). Leftmost fit (take the left child whenever it fits) is one condition smaller and also starts at offset 0, but it splits a larger free block while a smaller one fits elsewhere: with the left half free and one quarter of the right half free, a quarter-size acquire takes the left half and a later half-size acquire fails. Tile exhaustion aborts (tiled-data-stacks.md, "Tile exhaustion: abort naming the pool"), so the rule keeps large blocks whole. The test "Pool fills a smaller free block before splitting a larger one" fails under leftmost fit (`acquire(quarter)` returns 0, not 768).

## E2 — release takes the size

`release(offset, size_log2) -> bool` over `release(offset)` that descends to the `TAKEN` node. Reasons:

- Conform or declare (CLAUDE.md): the reference pair `bump_try_pop(void* p, std::size_t size)` (`src/pup/core/bump_alloc.cpp:152`) frees with the size the caller holds. `acquire(size_log2)` returning an offset and `release(offset, size_log2)` taking both back keeps that shape.
- The later `Tile` decommits 2^size_log2 bytes by its own field. With the size passed, the pool checks at one site that the Tile and the metadata agree; with `release(offset)` they can disagree silently.

## E3 — StackCursor is a pure value type, mutated in place

Candidates in the verdict: (i) a cursor `{ top, committed, reserved }` with the page passed per call, each result carrying the new state plus a commit target; (ii) `Stack` with the platform calls inline; (iii) (i) holding the page, validated with `reserved % page == 0`, each operation returning only the next cursor, and the edge deriving commit or decommit from the change in `committed`. The verdict chose (iii): over (i) because (i)'s page-aligned mark holds only if every call passes the same page, and its returned target and `committed` can disagree; over (ii) by CLAUDE.md "Pure core, effects at the edges", because (ii) can test its guards only against the real OS and its exhaustion path is an abort with nothing to assert on.

Attorney round 10 named a fourth candidate, (iii) mutated in place, and found it smaller: no `Push` result type and two fewer fields. Both are pure, and the edge still reads `committed()` before and after each call. It also conforms to the reference signatures: `push -> optional<offset>` like `bump_alloc` (with failure returned instead of an abort), `extend_top -> bool` like `bump_try_extend` (`bump_alloc.cpp:132`), `pop_top -> void` like `bump_try_pop` (`:152`). **Chosen: mutated in place.** The returning form was built first and replaced the same day.

The state carries data and every operation is legal in every state, so there is no state × event grid; laws over random sequences (exact commit mark after push, extend and decommit; unchanged cursor on a refused call) and boundary examples test it.

## E4 — Surface cut in round 10

Removed, with the attorney's reason (rounds 10 and 11):

- `StackCursor::operator==` and `page()`: only same-change tests called them.
- `tiled::Offset` and `offset.hpp`: the pool's offsets and a cursor's offsets are in different spaces, and the alias forbade neither mix. Both use `std::uint64_t`.
- Public `Pool::MAX_DEPTH`: its one reader is `pool.cpp`; it is a file-local constant there.
- `StackCursor::reserved()`: the `Stack` edge passes the tile size to `make` and holds the `Tile`, so `Stack::reserved()` reads the tile.
- `Pool` move assignment: no site assigns a `Pool`; it is deleted.

Kept and cleared: both `make` factories returning `optional` (validate once at construction; the pure core cannot call `fatal`), `acquire -> optional`, `release -> bool`, public constexpr `metadata_bytes` (sizes the static array at compile time), the `Pool` move constructor (`make` returns `optional<Pool>`; a move clears the source's metadata pointer, so a move does not leave two objects writing one array; two `make` calls over one span still do, and no test observes the clearing, because the only operations on a moved-from `Pool` dereference the null pointer).

Residual: a moved-from `Pool` holds a null metadata pointer, and `acquire` and `release` on it are not refused. Step 2 decides how `SpaceStorage` holds the `Pool`; if it never moves one after `make`, the residual has no call site.

## Behaviours

The verdict picked E1 and E2 on the condition that a bad release is reported, not a precondition; the implementer ruled it reported, and ruled the same for an out-of-range acquire (the verdict's Confirm items 1 and 2). The rest are the implementer's rulings or the verdict's findings, as marked.

- A bad release and an out-of-range acquire return failure; the edge turns failure into `fatal`.
- `make` rejects `pool_log2 ≥ 64`, `min_log2 > pool_log2`, a depth above 62 (the node count 2^(d+1) must fit in 64 bits), and a metadata span of the wrong size.
- `StackCursor::make` rejects a page that is zero or not a power of two, and `reserved % page != 0`.
- `push` rejects an alignment that is not a power of two (a precondition in `bump_alloc.cpp:116`).
- No operation wraps: bounds compare against `reserved − offset`, not `offset + n` (the verdict's finding: `bump_alloc.cpp:116-118` and `:138-139` wrap near the integer maximum).
- `decommit_above(keep)` never lowers the mark below the page-rounded top (implementer's ruling on the verdict's Confirm item 3; it keeps `top <= committed`). `Region::shrink(k)` therefore lowers the top to `min(top, k)` before it calls `decommit_above(k)`.
