# Tiled data stacks: an explicit memory layout for putup

Status: investigation, not built. Base: 94d99404 (main). Revision 13, settled: two image nodes on every platform; PIE kept on Linux. Attorney round 9: MINIMAL (2026-11-16).

## Requirements

- **R0 — power-of-two tiles** (user request, 2026-11-13): the address space is divided into tiles whose sizes are powers of two and whose addresses are aligned to their sizes.
- **R1 — learn from Goose data stacks** (user request, 2026-11-16 session). Goose: "A data stack is a large address-space reservation with a bump pointer"; "Allocation is `p = S.top; S.top += size`. Scope exit restores the watermark recorded at scope entry" (goose_spec.md §1.2, C.1).
- **R2 — based on the tiling system** (same request).
- **R3 — explicit layout** (same request): the application declares the arrangement of the tiles. Unlike Goose, the runtime does not choose it.
- **R4 — Region rebased** (same request): `Region` becomes a data stack on a tile of the tiling.
- **R5 — no std** (same request): the framework does not depend on the C++ standard library; putup's own primitives replace std types where needed. Scope settled by the user, 2026-11-16: all of putup, but header-only std types (`std::string_view`, `std::optional`, `std::span`, `std::variant`, `std::array`, …) stay for now and migrate only when one does not fit the memory framework. `STYLE.md:25-26` does not change. The framework itself links no std library code.
- **R9 — Goose inline nesting** (user request, 2026-11-16): investigate how Goose's inline nesting of variable-size values could apply here. Goose: a variable value is `[metadata][elements…]`, "inline, packed"; it is built "directly in its receiving storage"; "While a value is under construction on stack S, nothing else may allocate on S"; an array of variable-size elements is sequential, and random access uses a side array of `T&<u32 in pool>` (goose_spec.md §1.3(4), §3.3, §4.3, C.2).
- **R8 — Buf spill** (user decision, 2026-11-16, revised the same day): a spilled `Buf` gets its own data stack, a tile from the pool, as Goose gives each live resizable its own stack. The first ruling (grow on the heap stack top) broke `test/unit/test_buf.cpp:119`, `:131`, `:147`; the user then asked for more than one data stack, which removes the conflict. DESIGN.md :284-287 ("released when the Buf dies, unconditionally") stays true.
- **R11 — declared absolute base** (user decision, 2026-11-16): the layout names the address of the space. The executable image, the system libraries and the thread stack stay outside it: the OS places them (probe below).
- **R12 — the buddy root is the whole user address space** (user decision, 2026-11-16): the buddy tree's root spans the whole user address space from address 0. Every address, the image's included, lies in one node of one tree.
- **R13 — the image lies in declared nodes** (user decisions, 2026-11-16: the image is recorded in the layout as a range, and Linux keeps PIE): the layout names the nodes that may hold the executable image, as ranges, not exact addresses, so PIE and ASLR stay on every platform; every platform uses the same field and the same check. Each platform's layout names its own nodes (R15).
- **R15 — a layout per platform** (user decision, 2026-11-16): each platform may have its own layout; Windows has one now, and Linux and macOS may get their own when needed. The host selects one `Layout` constant per platform at compile time. Windows, macOS and Linux each have their own. macOS differs in pool size (see Layout and realization). The framework does not change: it already takes the layout as a template argument.
- **R14 — measured memory traffic** (user requirement, 2026-11-16): the memory traffic of the framework is measured against the allocator it replaces, and the design minimizes copying and moving of data, including copies that argument passing causes. Each migration step is compared with the allocator it replaces (`src/pup/core/bump_alloc.cpp`, `Region`) on the same workload; a step that adds copies or moves of data, or passes by value a type that does not fit in registers, records the reason. The metric and the tool are not chosen yet.
- **R6 — DESIGN.md § Memory Management:** "commits pages so n bytes are usable; aborts past the reservation" (:255); "Because the base never moves, both integer handles and raw pointers into a Region survive growth" (:260); "no `mremap` (macOS/Windows lack it)" (:247); `bump_try_extend` / `bump_try_pop` at the top (:269-270); "freshly committed pages are always zero" (:277); `Buf` spills to a private Region that is "released when the Buf dies, unconditionally" (:284-287).
- **R7 — `src/pup/core/bump_alloc.cpp:14-16`:** "operator new runs during static initialization and after static destruction, so the allocator must not depend on constructor or destructor ordering."
- **R10 — reusable outside putup** (user request, 2026-11-16): the framework has its own namespace, independent of `pup`, so other projects can reuse it; the platform primitives it needs are supplied by the host.

## Facts at the base

| Fact | Evidence |
|---|---|
| Single-threaded: no thread, mutex, atomic or `thread_local` in `src/` or `include/` | grep, 0 hits |
| 7 types own a `Region`; 22 declarations of Region-owning containers outside their own files, including `src/pup/cli/context.cpp:171-172`, `include/pup/graph/dag.hpp:30-31`, `include/pup/core/node_pair_set.hpp:56` | attorney grep |
| One `Graph` owns 37 Regions: 3 `StableVec`, 1 `Arena32`, `PathPool`'s `RobinHoodIndex`, and 2 `NodeIdArenaIndex` × 2 `NodeIdMap32` × 4 `IdArray32`, each with an `IdBitSet` | `dag.hpp:98`, `node_id_map.hpp:87`, `id_array.hpp:47`, `path_pool.hpp:101` |
| Every owner is default-constructible; `Buf` is default-constructed at about 193 local sites; 3 sites reset with `= Region {}` | `region.hpp:17`, `string_pool.cpp:120`, `robin_hood_index.cpp:56`, `:90` |
| macOS arm64 ignores `-no_pie`; two runs put `main` at `0x1041444f8` and `0x102a844f8`, libSystem at `0x191ea2d7c`, the stack near `0x16b…`; `mmap` with hint `2^45` for `2^44` bytes returned `0x200000000000` | scratchpad probe `aslr/p.c`, 2026-11-16 |
| Image placement by binary: macOS PIE at `0x1…` (probe); Linux shipped binary links `-static -no-pie` (`configs/linux.config:32`), so the linker default, about `0x400000`; Linux test binary PIE by toolchain default today (`configs/linux.config:28` has no `-no-pie`), about `0x55…`; Windows with lld-link's default high-entropy ASLR (`configs/xwin.config:29` names neither `/DYNAMICBASE` nor `/HIGHENTROPYVA`) `0x7ff7_c03b_d1d0`, in the high node (CI probe on `windows-latest`, one run, 2026-11-16; the CRT is linked statically and lies in the same image) | probe for macOS and Windows; linker and loader defaults for Linux, unmeasured: realization's image check aborts if a value is wrong |
| `Buf` spill sizes are not powers of two: `max(new_cap, 16 MiB)`, then `new_cap * 2` | `src/pup/core/buf.cpp:27`, `:31` |
| The Windows test binary allocates before `main`: it links `new_delete.cpp` (bump `operator new`), and Catch registers each test case through a namespace-scope `AutoReg` | `test/unit/Tupfile:19`, `src/pup/core/new_delete.cpp:29-33`, `third_party/catch_amalgamated.hpp:6147` |
| On POSIX the test binary takes `operator new` from the C++ runtime, not the bump region | `Tupfile:91-92` |
| `std::string_view`, `std::optional`, `std::variant`, `std::span` are project style, and stay (R5) | `STYLE.md:25-26` |
| macOS arm64 accepts a 2^45 aligned `PROT_NONE` reservation (2^46 × 2 refused); page 16 KiB | probe in session scratchpad, 2026-11-16 |
| xwin link flags name no `onecore.lib` / `mincore.lib` | `configs/xwin.config` |
| `Region` and the bump region reach putup through `pup/platform/vm.hpp` (5 free functions: `page_size`, `reserve`, `commit`, `decommit`, `release`), `pup/core/print.hpp` (fatal messages), and, for the bump region's stats, `pup/platform/env.hpp` | `src/pup/core/region.cpp:4-6`, `src/pup/core/bump_alloc.cpp:4-7`, `include/pup/platform/vm.hpp:10-27` |

## What transfers from Goose

| Goose mechanism | Here |
|---|---|
| Large reservation, commit on use, memory never moves | a `Stack` over a `Tile`; `Region` already has it |
| Free = restore the watermark | `pop_top` serves `bump_try_pop` on the heap stack; a spilled `Buf` returns its whole tile instead. A general `mark` / `rewind` waits for its first caller (attorney round 2) |
| One resizable per stack, on top | a growable owns its tile (`Region` owners); on the shared heap stack, growth in place only at the top, as `bump_try_extend` does now |
| N computed by the compiler | replaced by the application's `Layout` (R3) |
| Guard gap, no capacity check | not adopted (D1) |
| Static roots | not transferable |

## Design

```
Layout — constexpr { root_log2, image_nodes[2], base, pool_log2, heap_log2, min_tile_log2 } declared by the application; consteval checks
SpaceStorage<Layout> — the one constinit global: { optional<Pool>, metadata array }; the Pool is engaged at realization (reserve_at, image and page checks), on first use
Tile   — { Pool*, offset, size_log2 }; move-constructible, not assignable until Region (step 5) assigns one; minted by Tile::acquire(Pool&, size_log2), which calls fatal on exhaustion (the heap tile is the tree's first node: the first `bump_alloc` realizes the space and acquires it, from step 3); destructor decommits and returns it to the tree; base() is the Pool's base plus the offset
Pool   — buddy tree over the pool node, pure: acquire(size_log2) -> optional std::uint64_t offset inside the pool node; release(offset, size_log2) -> bool; move-only. It also holds the pool node's base address as an integer, checked aligned in make, so a Pool and its base travel as one record (attorney round 13); Tile reads it
StackCursor — the pure Stack arithmetic over std::uint64_t offsets in one tile: { top, committed, reserved, page }, mutated in place
Stack  — Tile + StackCursor; the edge reads committed() before and after each call and commits or decommits the difference: push, extend_top, pop_top, decommit_above
Region(reserve_bytes) — a Stack holding one value on a tile of the process pool
PackedTable<E> — length-prefixed sequences of E on one Region, plus an offset index (R9)
```

### Package boundary (R10)

- The framework has its own namespace (placeholder `tiled::`; the name is the user's) and its own include root. No framework header includes a `pup/` header.
- The framework declares the platform functions it requires and does not define them: `page_size`, `reserve_at(void* address, Size bytes) -> bool`, `commit`, `decommit`, `fatal(char const* message, Size length)`. The host links exactly one definition of each. The seam is entailed by `configs/linux.config:19-20`: the shipped Linux binary links no libc, so the framework cannot call `mmap` itself.
- `page_size`, `commit` and `decommit` mirror `pup/platform/vm.hpp:10-23`. Declared divergences: `reserve_at` replaces `reserve` (`vm.hpp:15`) because R11 fixes the address, and returns false when the OS does not give exactly that range; no `release` (`vm.hpp:27`), because nothing calls it: the space lives until process exit and a tile returns by decommit; and `fatal`, which `vm.hpp` does not have, because DESIGN.md :236-237 requires an abort with a message and the framework may not include `pup/core/print.hpp`. The framework formats its own numbers into the message (integer `std::to_chars`, header-only under R5).
- Putup's `vm-posix.cpp` and `vm-win32.cpp` define the seam functions in the framework's namespace. `pup::platform::vm` loses each function when its last caller goes: `page_size`, `commit` and `decommit` move to the seam in step 2; step 3 removes the bump region's `reserve` call (`bump_alloc.cpp:34`); step 4 removes `Region`'s reservation; step 5 removes `reserve`, `release` and `pup/platform/vm.hpp`. The tests that used them take a `Tile` from the process pool, and the `reserve_at` tests reserve one address per section and keep it until the process exits, as the pool node is kept. `test/unit/test_platform_vm.cpp` and `test_region.cpp:26`, `:77`, `:120`, `:152` move to the framework's names for the moved functions. No forwarding layer.
- The buddy tree and the stack arithmetic are a pure core over integer offsets and call no platform function. Only `SpaceStorage` realization (reserve_at, page_size, fatal), `Tile` (fatal in acquire, decommit in the destructor) and `Stack` (commit, decommit, fatal) call the seam.
- `PUP_BUMP_STATS` stays on putup's side of the boundary: putup's `operator new` wraps the heap `Stack` and keeps its per-site statistics.
- The host defines the space as one `constinit` global of the framework's storage type, instantiated with its layout: `constinit tiled::SpaceStorage<putup_layout> putup_space;`. That is how the host's constant reaches the framework with no run-time setup. Only the storage type takes the layout as a template argument; `Pool`, `Tile`, `Stack` and `Region` stay non-template.

### Layout and realization (R0, R2, R3, R7)

```cpp
inline constexpr auto putup_layout = tiled::Layout { .root_log2 = 47, .image_nodes = { { 0, 33 }, { 1ull << 46, 46 } }, .base = 1ull << 45, .pool_log2 = 44, .heap_log2 = 34, .min_tile_log2 = 24 };
```

- The application defines one layout constant per platform and selects it at compile time (R15); the constant below is the Linux one. The macOS one differs only in `pool_log2 = 40`: on macOS the cost of a `PROT_NONE` reservation grows linearly with its size, about 0.7 ms per TiB at `mmap` and as much again at `munmap` at exit (12.2 + 15.6 ms for 2^44, 1.5 + 1.1 ms for 2^40; measured 2026-11-16 on arm64). With 2^44, `putup --version` took a median of 32.5 ms against 4.2 ms with the old allocator; with 2^40, 6.9 ms against 5.2 ms. 2^40 still holds the heap tile and about a thousand 2^30 tiles. The space is `2^pool_log2` bytes and is the pool's buddy tree. At realization the tree marks its first `2^heap_log2` node as the heap tile, so the heap sits at offset 0, aligned to its size, and the rest of the space stays free for pool tiles. The consteval check requires `root_log2 <= 63`, `min_tile_log2 <= heap_log2 < pool_log2` and `base % 2^pool_log2 == 0`. The depth bound `pool_log2 − min_tile_log2 <= 62` (above 62 the node count `2^(d+1)` does not fit in 64 bits, and `Pool::metadata_bytes` returns a wrong size before `Pool::make` can refuse it) follows without its own check: a pool node of `2^63` is the whole root and cannot be disjoint from the image nodes. Alignment is measured from address 0, so it is a property of the declared literal, not of what the OS returns.
- The buddy root is `[0, 2^root_log2)`, the user address space (R12; 2^47 is the expected user space on x86-64 Linux, macOS and Windows, unmeasured here: a wrong value fails at `reserve_at`). The pool node `[base, base + 2^pool_log2)` is one node of that tree; every node off the path from the root to it is foreign: the OS owns it (image, libraries, thread stacks, page 0) and the tree never hands it out. The tree does not ask the OS where foreign mappings are; they are foreign because the layout does not declare them. Node arithmetic needs no base to subtract; no address-to-tile lookup is added until a caller needs one. The consteval checks add `base + 2^pool_log2 <= 2^root_log2`. The metadata array covers the pool node's subtree only (2 MiB above); the `root_log2 − pool_log2` levels above it hold no state, since their only non-foreign child is fixed by the literal.
- The space is realized on first use, from that constant. R7 rules out an explicit `init` call in `main` as the only entry: the Windows test binary allocates before `main`. The arrangement stays explicit; only the moment of reservation is lazy, as the bump region's is now. The user ruled on 2026-11-16 that a declared `constexpr Layout` with a lazy reservation meets R3.
- `SpaceStorage` holds the realized state and the `Pool` in one zero-initialized global, realized together on first use. The buddy tree's metadata is an array inside that global, sized at compile time from the layout: one byte per tree node, `2^(pool_log2 − min_tile_log2 + 1)` nodes, 2 MiB for the layout above, 128 KiB for the macOS one. It sits in zero-initialized static storage, so it needs no `commit` call and no seam function (an extra mapping would need an explicit commit under D1, which the pure tree cannot make). `min_tile_log2` bounds it; every pool tile requested today is at least 2^29 bytes (`region.hpp:15`, `id_bitset.hpp:41`).
- The space is reserved at exactly `base` (R11): `MAP_FIXED_NOREPLACE` on Linux, an address hint on macOS, `VirtualAlloc` at the address on Windows. Every POSIX definition compares the returned address with `base`, because Linux kernels before 4.17 treat `MAP_FIXED_NOREPLACE` as a hint. No 2× reservation and no `VirtualAlloc2`.
- `reserve_at` is a new function in both syscall backends beside `reserve` (`include/pup/platform/sys.hpp:85`; `src/pup/platform/sys-posix.cpp:222`, `src/pup/platform/sys-linux.cpp:334` for the shipped Linux binary) and in `vm-win32.cpp`.
- The image nodes are foreign nodes the layout names (R13): each is `{ base, log2 }`. The consteval checks add that each is aligned to its size, lies inside the root, and is disjoint from the pool node. Realization checks that the address of a function in the image lies inside one of them and aborts with the address otherwise. It catches nothing `reserve_at` would miss (an image over the pool node already makes the reservation fail); it gives the clearer message and puts the image's possible places in the layout. With one value everywhere it rejects only `[2^33, 2^46)`: macOS would accept an image in the high node and Windows one in the low node. The value is the same on every platform, so it needs no per-platform or per-binary selection; the shared `-fPIC` objects (`Tupfile:86`, `:104`) cannot tell the binaries apart, and do not need to.
- Realization checks `2^min_tile_log2 >= page_size()` and aborts otherwise; the page size is a run-time value (16 KiB on macOS arm64), so this is the one layout check that cannot be consteval.
- A sanitizer's shadow memory also takes fixed ranges: with ASan on macOS arm64, `[2^40, 2^44]` is taken and `2^45` is free (probed; the Linux bound is not measured). Test layouts follow the same constraint ([tiled-step2-decisions.md](tiled-step2-decisions.md), F4).
- A refused reservation, or one the OS places elsewhere, aborts with the layout's base and sizes in the message, as `region.cpp:76-80` aborts now. No halving probe: a run-time size different from the declared layout would contradict R3.

Assumed layout on x86-64 (not to scale; "probe" values measured on macOS arm64, "unmeas." values are loader defaults):

```
0x8000_0000_0000 ┬  2^47  root end
                 │ HIGH IMAGE NODE [2^46, 2^47)  foreign, declared: Windows exe 0x7ff7_c03b_d1d0 (probe); Linux libs/stack ~0x7f… ; Linux PIE ~0x5555_… (unmeas.)
0x4000_0000_0000 ┼  2^46
                 │ [2^45+2^44, 2^46)  foreign, undeclared
0x3000_0000_0000 ┼
                 │ POOL NODE  ours, reserve_at: pool tiles (min 2^24) for Regions and Buf spills
0x2004_0000_0000 ┄
                 │ heap tile [2^45, 2^45+2^34), first node: operator new
0x2000_0000_0000 ┼  2^45  .base
                 │ [2^33, 2^45)  foreign, undeclared; the image check rejects an image here
0x0002_0000_0000 ┼  2^33
                 │ LOW IMAGE NODE [0, 2^33)  foreign, declared: macOS libSystem 0x1_91ea_2d7c, stack 0x1_6bcb_987c, image 0x1_0414_44f8 (probe)
0x0001_0000_0000 ┄  2^32
                 │ macOS __PAGEZERO (unmapped); Linux static binary ~0x40_0000 (unmeas.)
0x0000_0000_0000 ┴  0
```

### Stack (R1, R4)

```cpp
class Stack {
public:
    explicit Stack(Tile tile);
    auto push(Size bytes, Size align) -> void*;
    auto extend_top(void* p, Size old_bytes, Size new_bytes) -> bool;
    auto pop_top(void* p, Size bytes) -> void;
    auto decommit_above(Size keep) -> void;
    auto base() const -> void*;
    auto top() const -> Size;
    auto committed() const -> Size;
    auto reserved() const -> Size;
};
```

- `push` commits explicitly and aborts past the tile (R6 :255).
- `extend_top` and `pop_top` conform to `bump_try_extend` (returns `bool`) and `bump_try_pop` (returns `void`).
- `extend_top` then `decommit_above` serve `Region::shrink(k)` (`test/unit/test_region.cpp:85-86`); see the table below.
- The observers serve `Region::data()`, `committed()`, `reserved()`, the bump statistics' net bytes (`bump_alloc.cpp:99`) and `bump_allocated_bytes()` (`bump_alloc.cpp:159`).

### Heap: operator new over the heap node

`bump_alloc` becomes `push` on the heap node's Stack, `bump_try_extend` becomes `extend_top`, `bump_try_pop` becomes `pop_top`, and `bump_allocated_bytes` becomes `top`. `Vec`, `SortedIdVec` and `Function` keep their behavior.

The layout constant is `pup::memory_layout` in `include/pup/core/memory_layout.hpp`. `bump_alloc.cpp` holds the space and the heap. R7 requires that `operator new` works after static destruction, so neither may have a destructor that runs at exit. `SpaceStorage` is trivially destructible, which `bump_alloc.cpp` asserts at compile time. The heap `Stack` has a destructor, through its `Tile`, so it is constructed in static storage on the first call and never destroyed. The first call acquires the heap tile before anything else can take a block from the pool, so the heap sits at offset 0.

### Region over a pool tile (R4)

| `Region` today | Over a tile |
|---|---|
| `Region(reserve_bytes)`, lazy reserve | `Region(reserve_bytes)`; acquires a tile of `max(2^min_tile_log2, next power of two ≥ reserve_bytes)` from `process_pool()` on the first `ensure` |
| `ensure(n)` | commit to `n` within the tile |
| `shrink(k)`, k > 0 | `extend_top(0, top, min(top, k))`, then `decommit_above(k)`: the cursor never decommits below its top; offset 0 holds because the Region's one value starts there |
| `shrink(0)` releases | the tile returns to the pool |
| `data()`, `committed()`, `reserved()` | tile base, commit mark, tile size |

Region stores `reserve_bytes` for lazy acquire and re-acquire after `shrink(0)`, and reads the pool through `pup::process_pool()` (decision D9 below). Owners move-assign Regions (`src/pup/core/arena.cpp:21`, `stable_vec.hpp:39`, `string_pool.cpp:120`, `robin_hood_index.cpp:56`, `:90`); a moved-from Region holds no tile. Every remaining reserve size is already a power of two (`region.hpp:15`, `id_bitset.hpp:41`).

### Buf (R8)

`Buf` keeps its `Region` member (`buf.hpp:83`), now a `Region` over the process pool; a Region is already a Stack on its own tile, and it already has the unspilled form (lazy acquire). Its 4 KiB inline buffer stays. On spill the Region acquires a tile of `max(2^min_tile_log2, next power of two ≥ new_cap)`, 16 MiB at the layout above, the same as `SPILL_RESERVE` now. Growth inside the tile commits in place, so the data pointer stays the same whatever other code allocates (`test_buf.cpp:131`). Growth past the tile replaces the Region with one twice as large and copies, as `buf.cpp:31` does now; the old Region's tile returns to the pool. The Region's destructor returns the last tile. The 193 default-constructed sites do not change: `Buf` acquires from the process space's pool, as it reserves from the OS now.

### std under R5

No std replacement is in this design. Header-only std types stay until one conflicts with the framework; none is known to. `std::unique_ptr` (22 uses) allocates through `operator new` and never frees, since every `delete` is already a no-op (`src/pup/core/throw_stubs.cpp:59`, DESIGN.md :270); the heap stack keeps that behaviour. `<algorithm>` is used only through `std::sort` and lookups, which take no buffer. Not replaceable in any case, because the compiler requires them by name: `std::initializer_list`, the `<compare>` ordering types used by `<=>`, placement `operator new`, `std::align_val_t`, `std::nothrow_t`.

### Packed records: Goose inline nesting (R9)

C++ has no variable-size types, so a packed value is bytes in a Region that code reads through a view. `PackedTable` is the one place that knows the byte layout.

**Mapping**

| Goose | PackedTable |
|---|---|
| `T[]`: "length field … then elements", inline, packed (§3.3, C.2) | a record is `[u32 len][pad to alignof(E)][E × len]`, appended at the top of the table's Region |
| sequential array + side index of `T&<u32 in pool>` (§3.3) | a `StableVec<u32>` of record offsets; a record's handle is its index, as `NodeId` and `StringId` are |
| built "directly in its receiving storage" (§4.3) | `allocate(len)` places the record at the top; `seq(r)` returns a writable span that the caller fills in place |
| nothing else allocates on S during construction (§1.3(4)) | the Region holds only this table's records, and `allocate` reserves the whole record before it returns |
| variable class: size fixed after construction | the length is fixed after `allocate`; elements stay writable in place |

**Declared divergences from Goose**

- Each record's elements start at `alignof(E)`. Goose uses unaligned loads; a typed load at a misaligned address fails the debug build's `-fsanitize=undefined` alignment check (`configs/debug.config:3-4`).
- No builder that streams elements through calls. Goose proves that nothing else touches the stack during such a build; in C++ that needs a lease state on the table that every append checks. No site found has an element count unknown before construction.
- One sequence per record, no head. Both sites that can use the table today have that shape. A head and several sequences per record (Goose's struct of variable parts) return when `CommandNode` gains a freeze point (see Sites).

**Interface**

```cpp
template<typename E>
class PackedTable {
public:
    PackedTable(Pool& pool, Size bytes_log2);
    auto allocate(std::uint32_t len) -> RecordId;
    auto seq(RecordId r) -> std::span<E>;
    auto size() const -> Size;
};
```

- `E` must be trivially copyable (`static_assert`): records hold no owning pointer and no destructor runs.
- `size()` conforms to the Region-owner cohort (`stable_vec.hpp:89`, `arena.hpp:50`, `string_pool.hpp:47`).
- The offset index is `u32`, so the constructor rejects `bytes_log2 > 32`.

**Sites at the base**

| Site | Shape today | Packing | Status |
|---|---|---|---|
| scheduler `dependents` | `Vec<Vec<std::size_t>>` (`scheduler.cpp:306`), filled by scattered `push_back` (`:379`), read-only after (`:383`, `:392`, `:408`, `:796`, `:860`, `:1072`) | a count pass, then `allocate` each job's record, then the scattered fill. Each job's `dependencies` list is computed twice or kept between the passes; step 5's measurement covers that cost | fits |
| builder group pool | `Vec<Vec<NodeId>>` (`builder.hpp:76`), members appended during parse | one record per group, after parse | needs a freeze point |
| `CommandNode` lists | nine sequences: `instruction` (`Vec<Atom>`), `exported_vars`, `guards`, and `inputs`, `order_only_inputs`, `outputs` as `TokenList`s of two `Vec`s each (`dag.hpp:70-80`, `token_list.hpp:161-162`) | one record per command with a head and nine sequences | needs a freeze point; `builder.cpp:3272` rewrites `instruction` after creation, through `get_command_node` (4 builder call sites: `:2133`, `:2514`, `:2609`, `:3212`) |
| `StringPool` | `bytes_` Region (NUL-terminated strings) + `entries_` of `{offset, length}` (`string_pool.hpp:63-73`) | a near relative: the length sits in the side entry, not in the record | no change needed |

Not a fit: `Arena32`, whose slices grow and move (Goose's `slice_pool` shape), and any nested list that grows interleaved with reads of it.

**Tests:** a roundtrip property (allocate random lengths, fill random elements, read every record back equal), and an alignment law (every `seq` data pointer is aligned to `alignof(E)`).

## Decisions

- **D1 — explicit commit with a bound check, not commit on fault.** R6 :255 "commits pages so n bytes are usable". Fault-driven commit fails "usable" on Windows: "runtime C writes to a stack only in user mode … and never through a system call, because a system call would fail on an uncommitted page instead of faulting" (implementation.md §7).
- **D2 — no relocation.** R6 :260 and :247.
- **D3 — a heap node at a declared place plus a buddy pool.** Fixed places alone fail DESIGN.md :291-292, "Reclamation within a run is limited to the bump top and whole-Region `shrink(0)`": a Region released and reserved again needs run-time acquire and release. A pool alone lets the buddy tree choose the heap's place, which R3 rules out (reading confirmed by the user, 2026-11-16).
- **D4 — move-only `Tile`, no generation counter.** Every tile has one owner holding it by value; the compiler checks it. CLAUDE.md: "Use the weakest tool that suffices".
- **D5 — the platform seam binds at link time.** R7: the space must serve allocations during static initialization, so the seam cannot need run-time setup; a run-time table of function pointers installed in `main` fails R7 for the Windows test binary. A template parameter (`Pool<Os>`) meets R7 but spreads into `Region<Os>` and every container that owns a Region, which R10's reuse does not require. Tests of the pure core need no seam; tests of `SpaceStorage`/`Tile`/`Stack` run against the real platform functions, as `test_region.cpp` does today.
- **D6 — the root is the address space, not the pool node.** R12. The alternative, a tree rooted at `base`, meets R0-R11 with one field fewer; R12 rules it out. The cost is one Layout field and one consteval check; the metadata does not grow.

- **D7 — lazy realization on every platform; no `_start` variant** (user decision, 2026-11-16: keep lazy realization on every platform, for consistency between platforms). Putup's own Linux `_start` (`src/pup/platform/crt0-linux.cpp:107`) could do three more things. It could realize the space before `run_init_array`, which drops the first-use check and the R7 concern. A static non-PIE link with `-Ttext=` could make the image a declared node. `_start` could also switch the main stack onto a declared stack tile. macOS needs `dyld` and libSystem and ignores `-no_pie` on arm64; the Windows test binary allocates before `main`. So the Linux path would diverge from the others. Revisit if a Linux-only gain is measured.
- **D8 — the image as a set of ranges.** Amended by R15: the two-node shape below is the Linux and macOS layout; Windows declares its own nodes. R13 with D7: an exact image address needs `-no_pie` (ignored on macOS arm64) or a custom `_start`. One node per platform fails R13's PIE clause on Linux: the shipped binary is static non-PIE (`configs/linux.config:32`, near `0x400000`), the test binary is PIE (near `0x55…`), they share `-fPIC` objects (`Tupfile:86`, `:104`; `configs/linux.config:14`), and the smallest aligned node holding both is the root. Two nodes, the low `[0, 2^33)` and the high `[2^46, 2^47)`, hold every placement in the facts table, are disjoint from the pool node `[2^45, 2^45 + 2^44)`, and need no link-flag change. Windows has its own layout constant. One measurement on `windows-latest` placed the image in the high node, so its values equal the Linux and macOS ones for now; a later Windows-only change does not touch the others.
- **D9 — Region reads the process pool; no pool parameter** (user decision, 2026-11-16). A `Pool&` parameter at every creator would make the pool an explicit input, but there is one pool per process and no caller has another one to pass. The inventory of 2026-11-16 found 21 classes that hold a `Region` by value, directly or through members, up to `Graph`, `BuildGraph`, `index::Index` and `BuildContext::Impl`: about 60 production and 230 test sites, against the 22 and 34 first counted. They would all pass the same pool and change no behavior. `Region` therefore reads `pup::process_pool()`, as `Buf` already did; a second pool is the trigger to revisit this.
- **D10 — a large allocation gets its own tile** (target chosen by the user, 2026-11-16, from the R14 measurement; threshold and mechanism are the maintainer's). `bump_alloc` serves a request of 16 KiB or more from its own pool tile of `max(2^min_tile_log2, next power of two ≥ size)`; `bump_try_extend` commits in place on that tile and `bump_try_pop` returns it. A heap value on top of the heap does not grow in place to 16 KiB or more: `bump_try_extend` refuses, so the value moves to its own tile once, with fewer than 16 KiB to copy (amended 2026-11-17: before, a value that grew on top passed the threshold on the heap and copied its whole size at its next move once something sat above it; `NodeIdPairSet`'s pairs copied 324 KB in 5 moves in a header-edit rebuild). Measured against the 16 KiB commit: header-edit rebuild copies 751728 → 446563 bytes, clean build 459995 → 442471, `putup parse` 137260 → 163384 and the null build 150192 → 176316 (each value that leaves the heap now copies once), while the bytes `putup parse` leaves on the heap fall from 2.40 MB to 0.64 MB. The caller's size is the tile's record, by the rule `Region` uses, and an address in the pool node above the heap tile marks a tile. This generalizes R8 to every caller of the bump API: Goose gives each live resizable its own stack. Copy attribution on the self-host clean build (2026-11-16) found 76% of the copied bytes in large `Vec` relocations (`Vec<index::EdgeEntry>` 3.1 MB in 11 moves, `Vec<pair<u32, u32>>` 2.1 MB in 13, `Vec<index::FileEntry>` 0.3 MB) and under 16 KiB in the scheduler's `dependents`, the first `PackedTable` site. Every site that replaces a bump block now returns the old one (`Vec::grow_to`, `Vec` copy assignment, `SortedIdVec`/`SortedPairVec` growth and move assignment); on the heap that is a no-op, on a tile it releases. 16 KiB (amended 2026-11-17 from 64 KiB, after the edge-list steps left `Vec` growth as most of the copying). Measured on the self-host tree at 64 / 32 / 16 KiB: clean-build copies 1085577 / 807813 / 459965 bytes, header-edit rebuild 937732 / 765087 / 751713, `putup parse` 204152 / 204152 / 137260. At 16 KiB a clean build holds at most 10 spill tiles at once (12 acquired) and a header-edit rebuild 11, against 2^16 tiles in the macOS pool. 16 KiB is one macOS page and a spill commits at least one page, so a smaller threshold wastes committed memory. (The first measurement, 64 against 256 KiB, gave 2.27 against 3.20 MB.) Residual: `operator new` (Windows, and the libc++ stubs in `throw_stubs.cpp`) never pops, so each large `operator new` holds its tile until exit, as it held its heap bytes before; a run with more than 2^(pool_log2 − min_tile_log2) of them aborts with "the pool has no free tile".

## Residuals

- `Region`, and so `Buf`, reads the process space's pool without a parameter (D9). It read the OS the same way before; every owner keeps its default construction.
- The declared base can be taken on a system whose loader or ASLR places something there, or lie above the user address space: a Linux aarch64 kernel with 39-bit addresses cannot map `1 << 45`, and CI has no aarch64 job. The binary then aborts at first allocation; no fallback.
- Growth of a heap value that is not on top: copies, until the value reaches 16 KiB; from then on it lives on its own tile (D10).
- Tile exhaustion: abort naming the pool, as "region reservation exhausted" does now.
- Zero on recommit and `PROT_NONE` above the commit mark come from the OS.

## Migration order

1. The pure core, under `include/tiled/` and `src/tiled/`, namespace `tiled`, with tests in `test/unit/test_tiled_pool.cpp` and `test_tiled_stack_cursor.cpp`. Build wiring: `Tupfile` (the `# tiled` sources), `test/unit/Tupfile`, the `Makefile` lint targets, and both bootstrap scripts. Built 2026-11-16, not committed. Encoding choices and the rulings behind them: [tiled-step1-decisions.md](tiled-step1-decisions.md).
   - `Pool`: the buddy tree over offsets. One metadata byte per node holds the node's order minus the largest free order in its subtree (0 = wholly free), with separate `FULL` and `TAKEN` values. `release(offset, size_log2)` reports a block that is not live. A move leaves the source without metadata. Tested by a model-based property against a set of disjoint aligned blocks, with release arguments drawn from all integers. A refused acquire must mean the model has no free aligned slot, and after all releases every metadata byte is zero.
   - `StackCursor`: the `Stack` arithmetic, mutated in place, with signatures that follow `bump_alloc`, `bump_try_extend` and `bump_try_pop`. `make` rejects a page that is zero or not a power of two, and `reserved % page != 0`. Every operation is legal in every state, so there is no state × event grid; laws over random sequences and boundary examples test it.
   - Reviewed by a different model (2026-11-16): one crash at depth 63 and five tests that passed for the wrong reason, all fixed. Design-attorney round 12 found the step-1 surface MINIMAL; rounds 10 and 11 cut `operator==`, `page()`, `reserved()`, `Pool` move assignment, the `Offset` alias and the public depth constant, and the cursor changed from returning the next cursor to mutating in place. After that change a mutation sweep killed 11 of 12 guard mutants in the normal build; the survivor (no size check in `release`) is killed under ASan and UBSan by "shift exponent -1 is negative", and the unmutated tests pass under both sanitizers.
2. The platform seam, `Layout` with `is_valid`, `SpaceStorage<Layout>`, `Tile` and `Stack` over `StackCursor`; `Pool` gains its base. Move `page_size`, `commit` and `decommit` from `pup::platform::vm` to the seam, add `reserve_at` to `sys.hpp:86` and both syscall backends, and move `test_platform_vm.cpp` and `test_region.cpp` to the new names.
3. The heap node replaces the bump region's own reservation. No caller changes. `pup::memory_layout` (`include/pup/core/memory_layout.hpp`) has a Windows, a macOS and a Linux branch (R15). Windows and Linux hold equal values after the image measurement in the facts table; macOS has a smaller pool after the reservation-cost measurement. Run the Windows test binary (CI `windows-latest`) at this step, since it is the binary that allocates before `main`.
4. `Region` takes its tile from the process pool, and so a spilled `Buf` sits on its own pool tile (R8). `test/unit/test_buf.cpp:119`, `:131`, `:147` stay unchanged and must pass. The Buf design below needs a `Region` over a pool tile, which the first order put in step 5; so this step moves `Region`'s internals and leaves its interface as it is. `Region` reads the pool through `pup::process_pool()`, without a parameter, as Residuals already allows for `Buf`; `process_pool()` acquires the heap tile first, so the heap stays at offset 0. A tile is `max(2^min_tile_log2, next power of two ≥ reserve_bytes)`, so `reserved()` reports the tile size (`test_region.cpp`, "Region custom reservation size"). Measured on macOS arm64 against step 3: `putup parse` copies the same 2796168 bytes; `putup --version` median 6.5 ms against 6.2 ms; gross heap bytes rise by 3320 because `Region` grows from 32 to about 72 bytes.
5. Remove `vm::reserve`, `vm::release` and `pup/platform/vm.hpp`; move `test_platform_vm.cpp` to the framework's names. The explicit `Region(Pool&, size_log2)` that this step first held is dropped (D9).
6. Large allocations spill to their own tile (D10). Measured on macOS arm64 against step 5, self-host workload: clean-build copies 7273417 → 2274249 bytes (−69%), heap gross 14.4 MB → 6.9 MB; `putup parse` copies unchanged (2796168: its copies are `Arena32` slice moves).
7. `Arena32` edge lists built in bulk (target chosen by the user, 2026-11-16: a counting build where every edge is known up front, `append_extend` kept for per-edge sites). Attribution showed the parse copies split evenly between `Index::add_edge`, called by the index reader for each edge, and `Index::build_edge_indices`, which the reader then calls and which rebuilt the same lists. The reader now appends the raw edges and builds once; the build counts each node's degree, `Arena32::allocate` sizes each list once to the next power of two (so a later `append_extend` keeps working), and `Arena32::write` fills it. `write` takes an index and asserts it lies inside the slice, rather than an in-place `append` that could not check the block's capacity. Measured against step 6: `putup parse` copies 2796168 → 204152 bytes (−93%); clean-build copies unchanged (2.27 MB), since a clean build loads no index; its remaining `Arena32` share (1.3 MB) was per-edge `add_edge` in `cmd_build`. Of that, 1.29 MB came from `process_implicit_deps`, which records discovered header dependencies one edge at a time while `reachable_from_producers` reads the index; that walk filters with `parsed_consumers`, which excludes `Implicit`, and starts only from command producers, so implicit edges are indexed in one build at the function's end without changing any query. `serialize_edges` builds once the same way. Clean-build copies 2274243 → 977671 bytes (−57%); the same 29 commands rebuild after a header edit. The carry-over steps that re-add a prior index's edges on an incremental build, `preserve_old_implicit_edges` and `merge_out_of_scope_commands`, read no edge list of the index they fill, and now build once at their end: a header-edit rebuild of the self-host tree copies 1925451 → 937732 bytes, with the same 29 commands. After this, no production code calls `Index::add_edge`.
8. If built (R9): `PackedTable<E>`. Its first site was the scheduler's `dependents`, which copies under 16 KiB on the self-host build, and a null build never builds it ("Nothing to do." returns before `execute_parallel`); a site must be chosen from the copy attribution first.
9. The group pool and `CommandNode` lists, after a freeze point after parse is confirmed; `CommandNode` brings back a head and several sequences per record.

## Open questions

- **Pool size** on Linux and Windows: the cost of reserving 2^44 there is unmeasured. macOS measured it and uses 2^40.
- **Freeze point.** Does any code write a command's `instruction`, `exported_vars` or `guards`, or a group's members, after parse ends? Step 6 depends on the answer.
- **Streaming builder.** Needed only if a site appears whose record lengths are unknown before construction.
