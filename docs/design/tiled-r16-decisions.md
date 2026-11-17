# R16 decisions: inline nested values in `tiled`

Requirement: R16 in [tiled-data-stacks.md](tiled-data-stacks.md). Design-attorney rounds 14 and 15 (2026-11-17) found the design below MINIMAL with two choices open; surface-compare settled them the same day. Each verdict names the evidence it rests on at d4d9a1d4.

## Settled by the attorney

- One `tiled::PackedStore<Head, Elems...>` per record kind, on one `tiled::Stack` over its own tile from a `Pool&`. A record is `[Head][u32 n0][E0 × n0][u32 n1][E1 × n1]…` with nothing inserted between parts.
- `RecordId` is the record's `u32` byte offset, minted only by `close`. The program keeps any dense index (R16 e).
- The builder is a typestate: `open(head)`, then `push` for the open sequence, `next() &&` to end it, and `close() &&` only after the last sequence. Order and completeness are compile errors (R16 b, f). Every illegal pair of the state × event grid is a `static_assert(!requires …)`.
- A sequence's count slot is reserved before its elements and written when its run ends (R16 b, maintainer's ruling).
- Reads are through a read-only `View`; a changed value is a new record (R16 d).
- Cut: a side index inside the store, parts at any position, sequences of variable-size elements and their nested builder, and a one-call `append` of finished lists (fails R16 b′).

## E6: what keeps other writers off the stack during a build

Picked: **a check in the builder, active when `NDEBUG` is not defined.** The builder keeps its own top; without `NDEBUG`, `push` compares it with the stack's top and calls the `fatal` seam when they differ, and in every build the stack's `extend_top` refuses the builder's next claim or its `close` (see the consequence below). This tests R16 (c) itself: any write to the stack by other code, including a second record opened inside a build. The store holds no lease state, so its layout and the builder's are the same in every build. A dropped builder leaves dead bytes that no `RecordId` reaches, and the next `open` is legal.

Rejected:
- A move-only write token that `open` consumes and `close` returns. clang-tidy's `bugprone-use-after-move` does not report a second move of a token held in a member (probe with clang-tidy 22.1.8), and the first site keeps the store in `Graph`, so the token detects nothing in the realistic violation, a command created inside another's build (`builder.cpp:2412`). A dropped builder also loses the token, and the store never opens again.
- Moving the `Stack` into the builder. Finished records are read during a build (`builder.cpp:2514-2515`), and a dropped builder destroys the tile with every finished record (`src/tiled/tile.cpp:42-51`).
- A lease flag in the store, checked by `assert`. Either the flag exists in release, a write per record that R16 (f) forbids, or the store's layout differs by `NDEBUG`, which breaks a host that mixes settings across translation units (R10); and a dropped builder leaves the flag set. (A function body that differs by `NDEBUG`, as the builder's check does, is the same formal ODR issue as any `assert` in a header; the layout is what differs.)
- Keeping the comparison in `next` and `close` in release (about four compares per record) so that no corrupted record gets a `RecordId`: it is bookkeeping beyond the bump pointers, which R16 (f) forbids.

Corrections from the different-model review (2026-11-17):
- `next` and `close` leave the moved-from builder with no stack, and `claim` and `close_run` call `fatal` on a builder with no stack, so a `push`, `next` or `close` on a spent builder stops in every build instead of rewriting the finished record's count (without this check a release build dereferences null, exit 139; verified 2026-11-17).
- The comparison is a pure function of the two tops, tested by enumeration as `StackCursor` is; the `fatal` call around it is the only untested line.
- A dropped builder rewinds the stack to the record's start, as Goose restores the watermark at scope exit (R1), so an abandoned build leaves no bytes. The rewind runs only when the builder is live and its top is the stack's top.
- Exclusivity by reachability, which is how Goose states the rule (§4.2: "The compiler rejects a growth it cannot show to be of a different array"): a host can keep the writable store away from the objects that callees receive. That is a host choice at each site, recorded with the first site.

Consequence for R16: the types enforce order, completeness and the layout (b, a, d). Exclusivity (c) cannot be enforced by a C++ type at a site that keeps the store in a long-lived object. It is enforced at run time in every build at no added cost: the builder writes only inside bytes it has claimed with `extend_top`, so another record opened on the same store lands above them, and the builder's next claim or its `close` is an `extend_top` that the cursor refuses because the top moved (`src/tiled/stack_cursor.cpp:55`); the builder then calls `fatal`. A release build therefore stops before any byte is overwritten, at the latest at `close`; a build dropped without `close` leaves its claimed bytes unused. Without `NDEBUG`, `push` also compares the tops, so the stop comes at the first push after the violation. Verified 2026-11-17 with `-O2 -DNDEBUG`: a nested record closed on the outer builder's store, then the outer `close`, and separately 100 outer pushes, each exit through `fatal`.

Rejected as a static proof of (c), after two different-model reviews (2026-11-17): a check link over the object files, where each growth references a mark symbol per region and each build interval is a root. ld64 removes a section's records even when its start symbol is referenced (`-dead_strip`, `section$start$…`: section size 0, link exit 0), `live_support` chains depend on link order, `used` records keep every target live, and `Function<Sig>` erases its callee into a thunk shared by all targets of one type, so a record at the erasure site cannot name the callee. A generated per-signature dispatch would need a second build configuration and a generator, and would still miss virtual calls and C callbacks. Goose proves (c) because its function values are compile-time only (goose_spec.md §7.6) and dispatch is closed (§8.2).

## E7: element alignment

Picked: **packed storage, natural element types, each element read by a copy**, with `static_assert(std::has_unique_object_representations_v<T>)` on `Head` and every element type, so a type with padding does not compile. A record may start at any byte offset; reads never form a reference into the store.

Rejected: one-byte-aligned element types read as `std::span`. `NodeId` is an alias of `std::uint32_t` (`include/pup/core/types.hpp:16`) and `StringId` an `enum class` (`include/pup/core/string_id.hpp:13`), so each would need a packed twin and a conversion at every push and read; and a reference bound to a packed member is undefined behavior that only UBSan reports (`src/pup/index/writer.cpp`, d4d9a1d4). Padding to `alignof(T)` fails R16 (a).

Declared divergence from d4d9a1d4, which reads the index records in place under `#pragma pack(1)`: those records are structs defined for the file; these elements are host types the framework cannot repack.

Elements are read with `std::bit_cast` from the stored bytes, since an element type need not be default-constructible (`Atom`, `include/pup/core/instruction.hpp:60-65`). The check rejects `float`, `double` and empty types (`has_unique_object_representations_v` is false for them); the framework does not support them until a site needs one, and a record always has a head.

The cost of a `push` in release is part of R16 (f): `StackCursor::push` divides by the page size on every call (`src/tiled/stack_cursor.cpp:11-17`) and is not inlined without LTO. The builder's `push` is inline and commits only when the top crosses the committed mark, with a mask for the page rounding.

Ruling on R16 (a): "packed" forbids bytes the store inserts. A field that an element type declares to remove its own padding, as `RawEdge::reserved` does (`include/pup/index/format.hpp`), is part of the element's value. `Atom` (2 padding bytes) and `Guard` (3) take such fields at the first site.

## First site: `CommandNode`

Not settled. The different-model review found three open decisions: what a command id names between its creation and its record's `close`, which `add_edge` and the owner check read (`src/pup/graph/dag.cpp:282-288`, `builder.cpp:2514-2515`; a rule that names one output twice already prints `already owned by command: cp  `, reproduced at d4d9a1d4); where the record opens relative to `process_generated_rules`, which reads the parent's position at `builder.cpp:2105`; and the record's content (six lists, `SourcePosition` in the head fails the padding check, token starts exist before the record opens). The framework is built first; the site is settled after.

Constraints found by both reviews, to be met at `src/pup/graph/builder.cpp`:
- The record opens after `process_generated_rules` (`:2412`), which creates other commands.
- Sequence order follows production order: inputs (`:2420`), outputs (`:2437`), order-only inputs (`:2567`).
- An operand and its token number are produced together, so each operand list is one sequence of pairs.
- `cmd_id` is empty in an inactive context (`:2386-2388`), and the loops still run, so the site splits that path.
- `parent_command` is `std::optional<NodeId>` and fails the padding check, so the head encodes "none" as a value.
