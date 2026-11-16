# Tiled data stacks, step 2: encoding decisions

Status: decision record for migration step 2 of [tiled-data-stacks.md](tiled-data-stacks.md), written 2026-11-16 from design-attorney round 13.

## F1 — where the pool's base address lives

A `Tile` must turn its pool offset into an address, and `Tile`, `Pool` and `Region` stay non-template (tiled-data-stacks.md, Package boundary), so a non-template object must hold the base beside the `Pool`.

| Candidate | Shape | Verdict |
|---|---|---|
| A | `Space { optional<Pool>, base }`, `Tile { Space*, offset, size_log2 }` | rejected: one type and one field more than C; `base` can be set while the Pool is empty, a combination that stands for no state |
| B | `Tile { Pool*, base, offset, size_log2 }`, minted by a friend `SpaceStorage<L>` | rejected: a `Pool&` and its base pass separately to every `Tile` and `Region` and can disagree (CLAUDE.md, "Values that always travel together are a record with a name") |
| C | `Pool` holds the base as an integer, checked aligned in `make`; `Tile { Pool*, offset, size_log2 }`, minted by `Tile::acquire(Pool&, size_log2)` | **chosen**: smallest of the three in types and fields with operations equal; no base exists outside an engaged `Pool`; keeps `Region(Pool&, size_log2)` |

C dominates A and B on the attorney's counts; no requirement fails it. `Tile` reads the base through friendship; a public `Pool::base()` would add an operation and make the choice a trade.

## F2 — `vm` shrinks per step

`pup::platform::vm` keeps `reserve` and `release` through step 2, because `bump_alloc.cpp:34` and `region.cpp:75` still call `reserve`, and `region.cpp:42`, `:58`, `:100` still call `release`; each goes when its last caller goes, which for both is `Region` in step 5 (step 3 removes only the bump region's `reserve` call). Removing them in step 2 breaks the build.

## F3 — `is_valid(Layout)` is a predicate

`SpaceStorage<L>` requires `is_valid(L)`; as a predicate a test can assert that a bad layout is rejected, which a check written inside the storage type would only show as a failed build.

## F4 — Declared bases avoid the sanitizer shadow

A `Layout` fixes its base at compile time, so the range must be free in every process that realizes it, including a sanitized one. Probed on macOS arm64 with Apple clang (`mmap` with an address hint, 1 GiB each): with ASan, every candidate in `[2^40, 2^44]` is taken; `0x180000000000`, `2^45`, `0x300000000000` and `2^46` are free; without ASan, all are free. Putup's base `2^45` lies above the probed range. For x86-64 Linux, the high shadow is recalled from LLVM's documentation to end near `0x10007fff8000`, which would also put `2^45` above it; that bound was neither looked up nor run here. The test layouts and the `reserve_at` test use `0x300000000000`, `0x310000000000` and `0x380000000000`; their first choices (`2^41`, `2^42`, `2^43`) passed a plain build and failed under ASan. `is_valid` cannot check this, because the shadow depends on the runtime; a taken range makes realization call `fatal` with the base.

## Residuals after review

- `Tile` decommits `2^size_log2` bytes, so a tile smaller than a page decommits its neighbours. `SpaceStorage` realization refuses `2^min_tile_log2 < page_size()`, which keeps every tile it hands out at least a page; a `Pool` built by `Pool::make` outside `SpaceStorage` has no such check.
- Windows image placement is unmeasured. `configs/xwin.config` links with lld-link's defaults (high-entropy ASLR, no fixed base). If the loader places the test binary between `2^33` and `2^46`, realization calls `fatal` ("the image lies outside the declared image nodes") and the `[tiled]` tests abort on the `windows-latest` runner. Step 2 changes no Windows link flag; a run on that runner settles it.
