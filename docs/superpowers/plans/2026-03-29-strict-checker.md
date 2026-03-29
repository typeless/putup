# Strict Convention Checker Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add `putup parse --strict` that verifies Tupfiles follow dual-mode composability conventions (2 error checks + 2 warning checks).

**Architecture:** Pure stateless check functions inspect AST nodes during evaluation via a new `on_statement` callback on `EvalContext`. Check results are `Vec<Diagnostic>`. The `cmd_parse` wiring collects diagnostics, prints them, and fails on errors.

**Tech Stack:** Existing AST types (`Assignment`, `Expression`, `VarRef`), `EvalContext` callback pattern, `Buf` for message formatting.

---

### Task 1: Add Diagnostic type and check_assignment function

**Files:**
- Create: `include/pup/cli/strict_checks.hpp`
- Create: `src/cli/strict_checks.cpp`

- [ ] **Step 1: Create the header with Diagnostic and check_assignment declaration**

```cpp
// include/pup/cli/strict_checks.hpp
// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include "pup/core/string_id.hpp"
#include "pup/core/vec.hpp"
#include "pup/parser/ast.hpp"

#include <cstddef>

namespace pup::cli {

struct Diagnostic {
    StringId file = StringId::Empty;
    std::size_t line = 0;
    enum Severity { Warning, Error } severity = Warning;
    StringId message = StringId::Empty;
};

/// Check an assignment statement for convention violations.
/// Only produces diagnostics for component Tuprules.tup files (not root).
[[nodiscard]]
auto check_assignment(
    parser::Assignment const& stmt,
    std::string_view file,
    bool is_component
) -> Vec<Diagnostic>;

/// Check component directories for filesystem-level conventions.
[[nodiscard]]
auto check_component_dirs(
    Vec<std::string_view> const& component_dirs
) -> Vec<Diagnostic>;

} // namespace pup::cli
```

- [ ] **Step 2: Create the implementation with check_assignment**

```cpp
// src/cli/strict_checks.cpp
// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/cli/strict_checks.hpp"
#include "pup/core/buf.hpp"
#include "pup/core/global_pool.hpp"
#include "pup/core/string_pool.hpp"
#include "pup/platform/file_io.hpp"

#include <variant>

namespace pup::cli {

namespace {

auto is_tuprules(std::string_view file) -> bool
{
    auto name = pup::path::filename(file);
    return name == "Tuprules.tup";
}

auto make_diag(
    std::string_view file,
    std::size_t line,
    Diagnostic::Severity severity,
    std::string_view msg
) -> Diagnostic
{
    auto& pool = global_pool();
    return Diagnostic {
        .file = pool.intern(file),
        .line = line,
        .severity = severity,
        .message = pool.intern(msg),
    };
}

// Known toolchain variable names
auto is_toolchain_var(std::string_view name) -> bool
{
    return name == "CC" || name == "CXX" || name == "AR" || name == "LD"
        || name == "AS" || name == "HOSTCC" || name == "HOSTCXX"
        || name == "RANLIB" || name == "STRIP" || name == "OBJCOPY" || name == "NM";
}

/// Check if an Expression is exactly [Variable{name}]
auto is_single_var(parser::Expression const& expr, std::string_view var_name) -> bool
{
    if (expr.parts.size() != 1) {
        return false;
    }
    auto const* var = std::get_if<parser::Expression::Variable>(&expr.parts[0]);
    if (!var) {
        return false;
    }
    return var->ref.kind == parser::VarRef::Kind::Regular
        && global_pool().get(var->ref.name) == var_name;
}

/// Check if an Expression contains a Variable with the given name
auto contains_var(parser::Expression const& expr, std::string_view var_name) -> bool
{
    for (auto const& part : expr.parts) {
        auto const* var = std::get_if<parser::Expression::Variable>(&part);
        if (var && var->ref.kind == parser::VarRef::Kind::Regular
            && global_pool().get(var->ref.name) == var_name) {
            return true;
        }
    }
    return false;
}

} // namespace

auto check_assignment(
    parser::Assignment const& stmt,
    std::string_view file,
    bool is_component
) -> Vec<Diagnostic>
{
    auto result = Vec<Diagnostic> {};

    // Only check component Tuprules.tup files
    if (!is_component || !is_tuprules(file)) {
        return result;
    }

    auto& pool = global_pool();
    auto name_sv = stmt.name.as_literal();
    if (name_sv.empty()) {
        return result; // Dynamic variable name — skip
    }

    // E1: S must use ?= in component Tuprules.tup
    if (name_sv == "S") {
        if (stmt.op != parser::Assignment::Op::SoftSet) {
            auto buf = Buf {};
            buf.fmt("'S' must use '?=' in component Tuprules.tup (found '='). "
                    "Using '=' overwrites the parent project's root anchor.");
            result.push_back(make_diag(file, stmt.location.line, Diagnostic::Error, buf.view()));
        } else if (!is_single_var(stmt.value, "TUP_CWD")) {
            auto buf = Buf {};
            buf.fmt("'S' should be set to '$(TUP_CWD)' in component Tuprules.tup.");
            result.push_back(make_diag(file, stmt.location.line, Diagnostic::Warning, buf.view()));
        }
        return result;
    }

    // E2: B must use ?= in component Tuprules.tup
    if (name_sv == "B") {
        if (stmt.op != parser::Assignment::Op::SoftSet) {
            auto buf = Buf {};
            buf.fmt("'B' must use '?=' in component Tuprules.tup (found '='). "
                    "Using '=' overwrites the parent project's build anchor.");
            result.push_back(make_diag(file, stmt.location.line, Diagnostic::Error, buf.view()));
        } else if (!contains_var(stmt.value, "S")) {
            auto buf = Buf {};
            buf.fmt("'B' should reference '$(S)' in its value.");
            result.push_back(make_diag(file, stmt.location.line, Diagnostic::Warning, buf.view()));
        }
        return result;
    }

    // W1: Toolchain variables should use ?=
    if (is_toolchain_var(name_sv) && stmt.op == parser::Assignment::Op::Set) {
        auto buf = Buf {};
        buf.fmt("'{}' should use '?=' in component Tuprules.tup. "
                "Using '=' overrides the parent project's toolchain choice.", name_sv);
        result.push_back(make_diag(file, stmt.location.line, Diagnostic::Warning, buf.view()));
    }

    return result;
}

auto check_component_dirs(
    Vec<std::string_view> const& component_dirs
) -> Vec<Diagnostic>
{
    auto result = Vec<Diagnostic> {};

    for (auto dir : component_dirs) {
        auto ini_path = global_pool().get(pup::path::join(dir, "Tupfile.ini"));
        if (!pup::platform::exists(ini_path)) {
            auto buf = Buf {};
            buf.fmt("Component directory '{}' has no Tupfile.ini — cannot be built standalone.", dir);
            result.push_back(make_diag(dir, 0, Diagnostic::Warning, buf.view()));
        }
    }

    return result;
}

} // namespace pup::cli
```

- [ ] **Step 3: Add to Tupfile**

Add `srcs-y += src/cli/strict_checks.cpp` to `Tupfile` after the other cli sources.

- [ ] **Step 4: Verify it compiles**

```bash
make 2>&1 | tail -5
```

Expected: Build succeeds. New code is not yet called.

- [ ] **Step 5: Commit**

```bash
git add include/pup/cli/strict_checks.hpp src/cli/strict_checks.cpp Tupfile
git commit -m "Add Diagnostic type and check_assignment/check_component_dirs functions"
```

---

### Task 2: Write unit tests for check functions

**Files:**
- Create: `test/unit/test_strict_checks.cpp`
- Modify: `test/unit/Tupfile`

- [ ] **Step 1: Write tests**

```cpp
// test/unit/test_strict_checks.cpp
// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "catch_amalgamated.hpp"
#include "pup/cli/strict_checks.hpp"
#include "pup/core/global_pool.hpp"
#include "pup/core/string_pool.hpp"

using namespace pup::cli;
using pup::StringId;
using pup::global_pool;
using pup::parser::Assignment;
using pup::parser::Expression;
using pup::parser::VarRef;

namespace {

auto intern(std::string_view s) -> StringId { return global_pool().intern(s); }
auto sv(StringId id) -> std::string_view { return global_pool().get(id); }

auto make_literal_expr(std::string_view text) -> Expression
{
    auto expr = Expression {};
    expr.parts.push_back(Expression::Literal { intern(text) });
    return expr;
}

auto make_var_expr(std::string_view var_name) -> Expression
{
    auto expr = Expression {};
    expr.parts.push_back(Expression::Variable { VarRef { VarRef::Kind::Regular, intern(var_name), {} } });
    return expr;
}

auto make_assignment(std::string_view name, Assignment::Op op, Expression value) -> Assignment
{
    auto stmt = Assignment {};
    stmt.name = make_literal_expr(name);
    stmt.op = op;
    stmt.value = std::move(value);
    stmt.location.line = 1;
    return stmt;
}

} // namespace

TEST_CASE("check_assignment: S anchor variable", "[strict]")
{
    SECTION("S ?= $(TUP_CWD) in component — no diagnostic")
    {
        auto stmt = make_assignment("S", Assignment::Op::SoftSet, make_var_expr("TUP_CWD"));
        auto diags = check_assignment(stmt, "libfoo/Tuprules.tup", true);
        REQUIRE(diags.empty());
    }

    SECTION("S = $(TUP_CWD) in component — error")
    {
        auto stmt = make_assignment("S", Assignment::Op::Set, make_var_expr("TUP_CWD"));
        auto diags = check_assignment(stmt, "libfoo/Tuprules.tup", true);
        REQUIRE(diags.size() == 1);
        REQUIRE(diags[0].severity == Diagnostic::Error);
        REQUIRE(sv(diags[0].message).find("must use '?='") != std::string_view::npos);
    }

    SECTION("S = $(TUP_CWD) in root — no diagnostic (exempt)")
    {
        auto stmt = make_assignment("S", Assignment::Op::Set, make_var_expr("TUP_CWD"));
        auto diags = check_assignment(stmt, "Tuprules.tup", false);
        REQUIRE(diags.empty());
    }

    SECTION("S ?= literal in component — warning")
    {
        auto stmt = make_assignment("S", Assignment::Op::SoftSet, make_literal_expr(".."));
        auto diags = check_assignment(stmt, "libfoo/Tuprules.tup", true);
        REQUIRE(diags.size() == 1);
        REQUIRE(diags[0].severity == Diagnostic::Warning);
    }
}

TEST_CASE("check_assignment: B anchor variable", "[strict]")
{
    SECTION("B ?= with $(S) in component — no diagnostic")
    {
        auto expr = Expression {};
        expr.parts.push_back(Expression::Variable { VarRef { VarRef::Kind::Regular, intern("TUP_VARIANT_OUTPUTDIR"), {} } });
        expr.parts.push_back(Expression::Literal { intern("/") });
        expr.parts.push_back(Expression::Variable { VarRef { VarRef::Kind::Regular, intern("S"), {} } });

        auto stmt = make_assignment("B", Assignment::Op::SoftSet, std::move(expr));
        auto diags = check_assignment(stmt, "libfoo/Tuprules.tup", true);
        REQUIRE(diags.empty());
    }

    SECTION("B = ... in component — error")
    {
        auto stmt = make_assignment("B", Assignment::Op::Set, make_literal_expr("build"));
        auto diags = check_assignment(stmt, "libfoo/Tuprules.tup", true);
        REQUIRE(diags.size() == 1);
        REQUIRE(diags[0].severity == Diagnostic::Error);
    }
}

TEST_CASE("check_assignment: toolchain variables", "[strict]")
{
    SECTION("CC ?= gcc in component — no diagnostic")
    {
        auto stmt = make_assignment("CC", Assignment::Op::SoftSet, make_literal_expr("gcc"));
        auto diags = check_assignment(stmt, "libfoo/Tuprules.tup", true);
        REQUIRE(diags.empty());
    }

    SECTION("CC = gcc in component — warning")
    {
        auto stmt = make_assignment("CC", Assignment::Op::Set, make_literal_expr("gcc"));
        auto diags = check_assignment(stmt, "libfoo/Tuprules.tup", true);
        REQUIRE(diags.size() == 1);
        REQUIRE(diags[0].severity == Diagnostic::Warning);
    }

    SECTION("CC = gcc in root — no diagnostic")
    {
        auto stmt = make_assignment("CC", Assignment::Op::Set, make_literal_expr("gcc"));
        auto diags = check_assignment(stmt, "Tuprules.tup", false);
        REQUIRE(diags.empty());
    }

    SECTION("CFLAGS = -O2 in component — no diagnostic (not toolchain)")
    {
        auto stmt = make_assignment("CFLAGS", Assignment::Op::Set, make_literal_expr("-O2"));
        auto diags = check_assignment(stmt, "libfoo/Tuprules.tup", true);
        REQUIRE(diags.empty());
    }
}

TEST_CASE("check_assignment: non-Tuprules file — no diagnostic", "[strict]")
{
    auto stmt = make_assignment("S", Assignment::Op::Set, make_var_expr("TUP_CWD"));
    auto diags = check_assignment(stmt, "libfoo/Tupfile", true);
    REQUIRE(diags.empty());
}

TEST_CASE("check_component_dirs", "[strict]")
{
    SECTION("existing directory with Tupfile.ini — no diagnostic")
    {
        // Use the project root which has Tupfile.ini
        auto dirs = pup::Vec<std::string_view> { "." };
        auto diags = check_component_dirs(dirs);
        REQUIRE(diags.empty());
    }

    SECTION("directory without Tupfile.ini — warning")
    {
        auto dirs = pup::Vec<std::string_view> { "/tmp/nonexistent_dir_for_test" };
        auto diags = check_component_dirs(dirs);
        REQUIRE(diags.size() == 1);
        REQUIRE(diags[0].severity == Diagnostic::Warning);
    }
}
```

- [ ] **Step 2: Add to test Tupfile**

Add `test-srcs-y += test_strict_checks.cpp` after the existing test sources in `test/unit/Tupfile`.

- [ ] **Step 3: Build and run tests**

```bash
make 2>&1 | tail -5
./build/test/unit/putup_test '[strict]' -s 2>&1 | tail -10
```

Expected: All strict tests pass.

- [ ] **Step 4: Commit**

```bash
git add test/unit/test_strict_checks.cpp test/unit/Tupfile
git commit -m "Add unit tests for strict convention checks"
```

---

### Task 3: Add on_statement callback to EvalContext

**Files:**
- Modify: `include/pup/parser/eval.hpp`

- [ ] **Step 1: Add the callback**

Add after the existing `on_env_var_used` callback (around line 156):

```cpp
    /// Callback for strict convention checking.
    /// Called with each statement and the directory it belongs to.
    Function<void(parser::Statement const&, std::string_view dir)> on_statement = {};
```

- [ ] **Step 2: Fire the callback in the evaluator**

In `src/graph/builder.cpp`, find where statements are processed. Look for the loop that iterates over `tupfile.statements` (in the function that evaluates a parsed Tupfile). Add at the top of the loop body:

```cpp
if (ctx.eval->on_statement) {
    ctx.eval->on_statement(stmt, current_dir);
}
```

The exact location depends on the builder's evaluation loop. Read `builder.cpp` to find where each `Statement` is dispatched to `process_assignment`, `process_rule`, etc. Fire the callback before the dispatch.

- [ ] **Step 3: Verify it compiles and tests pass**

```bash
make 2>&1 | tail -5
./build/test/unit/putup_test '~[e2e]' 2>&1 | tail -3
```

Expected: Build succeeds, all tests pass (callback is never set yet).

- [ ] **Step 4: Commit**

```bash
git add include/pup/parser/eval.hpp src/graph/builder.cpp
git commit -m "Add on_statement callback to EvalContext for strict checking"
```

---

### Task 4: Wire --strict flag into cmd_parse

**Files:**
- Modify: `include/pup/cli/options.hpp`
- Modify: `src/cli/cmd_parse.cpp`
- Modify: `src/cli/options.cpp` (if arg parsing is there)

- [ ] **Step 1: Add strict flag to Options**

Add to the `Options` struct in `options.hpp`:

```cpp
    bool strict = false;
```

- [ ] **Step 2: Parse --strict from command line**

Find where `parse` command arguments are parsed (likely `src/cli/options.cpp`). Add `--strict` handling that sets `opts.strict = true`.

- [ ] **Step 3: Wire the checker into parse_single_variant**

In `src/cli/cmd_parse.cpp`, modify `parse_single_variant`:

```cpp
#include "pup/cli/strict_checks.hpp"

// ... inside parse_single_variant, before build_context():

auto diagnostics = Vec<Diagnostic> {};
auto component_dirs = Vec<std::string_view> {};

if (opts.strict) {
    auto source_root_sv = pool.get(layout->source_root);

    ctx_opts.on_statement = [&](parser::Statement const& stmt, std::string_view dir) {
        if (!stmt.is<parser::Assignment>()) {
            return;
        }
        auto const& assign = stmt.get<parser::Assignment>();

        // Determine if this is a component (has own Tuprules.tup, not root)
        auto is_component = !dir.empty() && dir != "." && dir != source_root_sv;

        auto file_sv = /* reconstruct Tuprules.tup path from dir */;
        auto diags = check_assignment(assign, file_sv, is_component);
        for (auto& d : diags) {
            diagnostics.push_back(std::move(d));
        }
    };
}

// ... after build_context(), before printing graph:

if (opts.strict) {
    // Collect component dirs (dirs with Tuprules.tup that aren't root)
    // ... detect from parsed_dirs or builder state

    auto fs_diags = check_component_dirs(component_dirs);
    for (auto& d : fs_diags) {
        diagnostics.push_back(std::move(d));
    }

    // Print diagnostics
    auto has_errors = false;
    for (auto const& d : diagnostics) {
        auto severity_str = d.severity == Diagnostic::Error ? "error" : "warning";
        fprintf(stderr, "%s:%zu: %s: %s\n",
            pool.get(d.file).data(),
            d.line,
            severity_str,
            pool.get(d.message).data());
        if (d.severity == Diagnostic::Error) {
            has_errors = true;
        }
    }

    if (has_errors) {
        return EXIT_FAILURE;
    }
}
```

Note: The exact wiring depends on how `build_context` exposes parsed directory info and how the `on_statement` callback receives the current file path. Read `builder.cpp` and `context.cpp` to determine the precise integration points.

- [ ] **Step 4: Build and test**

```bash
make 2>&1 | tail -5
./build/test/unit/putup_test '~[e2e]' 2>&1 | tail -3
```

- [ ] **Step 5: Commit**

```bash
git add include/pup/cli/options.hpp src/cli/options.cpp src/cli/cmd_parse.cpp
git commit -m "Wire --strict flag into putup parse command"
```

---

### Task 5: Add E2E test

**Files:**
- Create: `test/e2e/fixtures/strict_check/`
- Modify: `test/unit/test_e2e.cpp`

- [ ] **Step 1: Create a fixture that violates conventions**

```bash
mkdir -p test/e2e/fixtures/strict_check/libfoo
```

`test/e2e/fixtures/strict_check/Tupfile.fixture`:
```
: |> echo ok |>
```

`test/e2e/fixtures/strict_check/Tuprules.tup.fixture`:
```
S = $(TUP_CWD)
CC = gcc
```

`test/e2e/fixtures/strict_check/libfoo/Tuprules.tup.fixture`:
```
S = $(TUP_CWD)
CC = gcc
```

`test/e2e/fixtures/strict_check/libfoo/Tupfile.fixture`:
```
: |> echo libfoo |>
```

Note: libfoo uses `=` instead of `?=` for S and CC, and has no Tupfile.ini.

- [ ] **Step 2: Write E2E test**

```cpp
SCENARIO("Strict checker catches convention violations", "[e2e][strict]")
{
    GIVEN("a project with a component violating conventions")
    {
        auto f = E2EFixture { "strict_check" };
        REQUIRE(f.init().success());

        WHEN("parse --strict is run")
        {
            auto result = f.pup({ "parse", "--strict" });

            THEN("it fails with error diagnostics")
            {
                REQUIRE_FALSE(result.success());
                REQUIRE(result.stderr_output.find("must use '?='") != std::string::npos);
            }
        }

        WHEN("parse without --strict is run")
        {
            auto result = f.pup({ "parse" });

            THEN("it succeeds (no strict checking)")
            {
                REQUIRE(result.success());
            }
        }
    }
}
```

- [ ] **Step 3: Build and run**

```bash
make 2>&1 | tail -5
./build/test/unit/putup_test '[strict]' -s 2>&1 | tail -10
```

Expected: Both unit and E2E strict tests pass.

- [ ] **Step 4: Commit**

```bash
git add test/e2e/fixtures/strict_check/ test/unit/test_e2e.cpp
git commit -m "Add E2E test for --strict convention checker"
```

---

### Task 6: Update bootstrap scripts and documentation

**Files:**
- Modify: `bootstrap-linux.sh`, `bootstrap-macos.sh`, `bootstrap-mingw.sh`
- Modify: `docs/reference.md`

- [ ] **Step 1: Add strict_checks.cpp to bootstrap scripts**

Add compile + link entries for `src/cli/strict_checks.cpp` → `build/strict_checks.o` in all three bootstrap scripts. Add `build/strict_checks.o` to the link and ar lines.

- [ ] **Step 2: Document --strict in reference.md**

Add to the `parse` subcommand section:

```markdown
**Convention checking (`--strict`)**

Verify Tupfiles follow dual-mode composability conventions:

```bash
putup parse --strict
```

Checks that component libraries (directories with their own `Tuprules.tup`) follow conventions that allow building both standalone and as part of a larger project:

- **Error:** Anchor variables `S` and `B` must use `?=` (not `=`) in component `Tuprules.tup`
- **Warning:** Toolchain variables (`CC`, `CXX`, `AR`, etc.) should use `?=`
- **Warning:** Component directories should contain `Tupfile.ini` for standalone builds

Exit code is non-zero if any errors are found. Warnings print but don't fail.
```

- [ ] **Step 3: Build, test, format**

```bash
make 2>&1 | tail -5
./build/test/unit/putup_test 2>&1 | tail -3
make format 2>&1 | tail -3
```

- [ ] **Step 4: Commit**

```bash
git add bootstrap-linux.sh bootstrap-macos.sh bootstrap-mingw.sh docs/reference.md
git commit -m "Update bootstrap scripts and docs for --strict flag"
```
