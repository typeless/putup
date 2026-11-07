---
name: contributing
description: Contributing to putup development. Use when building putup, running tests, writing E2E tests, understanding the architecture, or following the code style.
---

# Contributing to Putup

Putup is a build system using Tup's Tupfile syntax, written in C++20. It builds itself (self-hosting) and requires `putup` in PATH.

The repository documents each topic once; this skill points to those files (paths relative to the repository root, <https://github.com/typeless/putup>):

| Topic | File |
|-------|------|
| Make targets, TDD workflow, platform configs, `CONFIG=xwin` | `CLAUDE.md` (Building & Testing, Development Workflow) |
| Test commands, tags, `test/runner/`, E2E fixtures, `E2EFixture` API, shell fixtures, debugging, coverage, Windows tests | `TESTING.md` |
| C++ style, project primitives, checklist | `STYLE.md` |
| Architecture, index format, dependency scanning | `DESIGN.md` |
| Requirements format and `discharge:` citations | `spec/requirements/README.md` |
| User-facing commands and Tupfile syntax | `docs/reference.md` |

## Commands

```bash
make                                   # configure + build into build/
make test                              # unit + E2E tests, run as graph rules in test/runner/
./build/test/unit/putup_test '[tag]'   # one tag, direct
make format && make tidy && make iwyu  # before every commit
make spec-check                        # requirements against the test suite
make bootstrap                         # regenerate bootstrap-linux.sh / bootstrap-macos.sh
make check                             # format-check + spec-check + test
```

## Obligations

- **Tests first.** Write the failing test, run it, observe the failure, then implement. Steps: `CLAUDE.md` (Development Workflow); fixtures and test layout: `TESTING.md` (Adding New Tests).
- **`make iwyu`** runs `clang-include-cleaner-18` and fails on any dead include or unanalyzable file. The macOS override (`CLANG_INCLUDE_CLEANER`, `IWYU_FLAGS`) is in `CLAUDE.md` (Building & Testing).
- **`make bootstrap`** after any change to `Tupfile`, `Tuprules.tup`, or `configs/`; commit the regenerated `bootstrap-*.sh`. CI's `bootstrap-drift` job (`.github/workflows/ci.yml`) fails on stale scripts.
- **clang-format 18.** CI's `format-check` job runs `make format-check` on `ubuntu-latest`, which ships clang-format 18. Other major versions format differently; check with an 18.x clang-format.
- **Spec-check.** When a change alters behaviour that `spec/requirements/` specifies, update the requirement and run `make spec-check`. Requirements cite tests by name (`discharge: test "Scenario: ..."`), so renaming or deleting a cited test is a spec change. Format: `spec/requirements/README.md`; example: `TESTING.md` (Update the Spec).

## Differential Testing Against Real Tup

For semantic questions ("what should this Tupfile do?"), real tup is the
reference. Build the same source tree with both and diff the output trees:

```bash
# Same source dir, two variants — identical commands, comparable bytes
tup variant configs/<board>.config && tup
putup configure --config configs/<board>.config -B build-putup && putup -B build-putup

cd build-<board> && find . -type f | grep -v '^./\.tup' | sort > /tmp/a
cd ../build-putup && find . -type f | grep -v '^./\.pup' | sort > /tmp/b
diff /tmp/a /tmp/b                  # same file set?
# then cmp matching pairs for bit-identity
```

- Run both from the **same source directory**: `-g` embeds the absolute
  compile dir (`DW_AT_comp_dir`), so separate checkouts differ spuriously.
- Variant directory names leak into outputs via `%f` paths (e.g.
  `objdump -S %f` prints the input path) — normalize before diffing.
- A stale-but-plausible output (wrong bytes, never rebuilt) means putup ran a
  different command than tup: compare `putup show index PATTERN` against the
  rule to find the divergence.
- Neutralize the environment: fix `SOURCE_DATE_EPOCH`, and defeat compiler
  caches (projects that wrap commands via `import CCACHE_EXEC=` run raw with
  `CCACHE_EXEC=` set empty).

This found the conditional bang-macro scoping bug: 1830-file trees diffed in
exactly 3 files, and the index showed putup had executed the other `ifeq`
branch's `!hex_cat`.

## Project Structure

```
pup/
  include/pup/
    cli/          # Command-line interface, options, output
    core/         # Core types, hash, result, primitives
    parser/       # Lexer, parser, AST, evaluator, depfile
    graph/        # Dependency DAG, builder, topological sort, rule patterns
    index/        # Binary index format, reader/writer
    exec/         # Scheduler, command runner
    platform/     # OS layer: env, file I/O, processes, sys, virtual memory
  src/            # Implementation files (mirrors include/pup/ layout) + main.cpp
  configs/        # Per-platform configs, selected via CONFIG env var
  test/
    unit/         # Catch2 tests (test_*.cpp) + e2e_fixture.{hpp,cpp}
    e2e/fixtures/ # E2E fixture data
    runner/       # Nested project that runs the test binary as graph rules
  spec/requirements/  # EARS requirements checked by make spec-check
  third_party/    # expected.hpp (expected-lite), sha256/, catch_amalgamated.{hpp,cpp}
  Makefile        # Workflow wrapper
  Tupfile         # Build configuration
  Tuprules.tup    # Shared build rules
```

## Design Principles

- **No libstdc++** -- links with `-nostdlib++`; containers, strings, and callables use in-house primitives listed in `STYLE.md` (Project Primitives)
- **Git-inspired index** -- binary format, not SQLite; SHA-256 content hashing
- **No FUSE** -- dependencies come from Tupfile declarations plus putup's own dependency scan, which runs the compiler's `-M` and records discovered headers as implicit edges; commands that already pass `-MD` are skipped (`DESIGN.md`, Why No FUSE? and Execution Integration)
- **No Lua** -- pure Tupfile syntax, no scripting extensions
- **Result<T>** -- fallible operations return `Result<T>`; builds use `-fno-exceptions`
- **Design coherence** -- no workarounds. If something does not fit the architecture, fix the design.
