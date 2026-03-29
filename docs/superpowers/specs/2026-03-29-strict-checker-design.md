# Strict Convention Checker for Dual-Mode Composability

## Goal

Add `putup parse --strict` that verifies Tupfiles follow conventions guaranteeing a project can be built both standalone and as a component of a larger project.

## Motivation

A library project with proper conventions can be built independently (`cd libfoo && putup -B build`) or as part of a larger project (`cd big-project && putup -B build`) with zero Tupfile changes. The conventions rely on `?=` defaults, anchor variables, and prefixed directory variables. Violations are silent -- the project builds fine standalone but breaks when composed. `--strict` catches these violations early.

## Conventions Checked

### Errors (non-zero exit)

**E1: Anchor variable `S` must use `?=` in component Tuprules.tup.**

`S ?= $(TUP_CWD)` sets the project-root anchor. With `=`, the component overwrites the parent's value during `include_rules` (root-to-leaf), breaking all `$(S)/...` path references in composed mode.

Detection: Assignment AST where name is `"S"`, in a component Tuprules.tup (not root). Check that `op == SoftSet` and value Expression is `[Variable{TUP_CWD}]`.

**E2: Anchor variable `B` must use `?=` in component Tuprules.tup.**

`B ?= $(TUP_VARIANT_OUTPUTDIR)/$(S)` sets the build-root anchor. Same rationale as E1.

Detection: Assignment AST where name is `"B"`, in a component Tuprules.tup. Check that `op == SoftSet` and value Expression contains `Variable{S}`.

### Warnings (print but don't fail)

**W1: Toolchain variables should use `?=` in component Tuprules.tup.**

Variables `CC`, `CXX`, `AR`, `LD`, `AS`, `HOSTCC`, `HOSTCXX`, `RANLIB`, `STRIP`, `OBJCOPY`, `NM` with `=` in a component override the parent's toolchain choice.

Detection: Assignment AST for known toolchain names, in a component Tuprules.tup. Check `op == SoftSet`.

**W2: Component directory should contain `Tupfile.ini`.**

Without `Tupfile.ini`, the component cannot be built standalone.

Detection: Filesystem check after evaluation. Each component directory (has own Tuprules.tup, is not root) should contain `Tupfile.ini`.

### Future phases (not in initial implementation)

- `*_DIR` variables should use `?=` with default `.` in components
- `defaults.config` should exist in component directories
- Cross-directory group references should use anchored paths (`$(S)/$(DIR_VAR)/<group>`)
- Rule input/output paths should not contain hardcoded `..` in Literal AST parts

## Architecture

### Data types

```cpp
struct Diagnostic {
    StringId file;
    std::size_t line;
    enum Severity { Warning, Error } severity;
    StringId message;
};
```

### Check functions (pure, stateless)

```cpp
// Called per Assignment statement during evaluation
auto check_assignment(
    parser::Assignment const& stmt,
    std::string_view file,
    std::string_view dir,
    bool is_component
) -> Vec<Diagnostic>;

// Called after evaluation for filesystem-level checks
auto check_component_dirs(
    ProjectLayout const& layout,
    Vec<std::string_view> const& component_dirs
) -> Vec<Diagnostic>;
```

Each function takes input, returns diagnostics. No hidden state, no class.

### Callback integration

Add one callback slot to `EvalContext`:

```cpp
Function<void(Statement const&, std::string_view dir)> on_statement = {};
```

This fires for every statement during Tupfile evaluation. The `cmd_parse` wiring:

1. Before `build_context()`: wire `on_statement` to call `check_assignment` for Assignment statements, append results to a `Vec<Diagnostic>`
2. Track which directories have their own `Tuprules.tup` (component detection)
3. After `build_context()`: call `check_component_dirs` for filesystem checks
4. Print diagnostics to stderr, return non-zero if any Error-severity diagnostics exist

### Component detection

A directory is a **component** if:
- It has its own `Tuprules.tup`
- It is not the project root (source_root)

The root project is exempt from all checks -- it IS the authority for anchor variables and toolchain settings.

### Severity escalation

`--strict` uses natural severity (errors fail, warnings print). A future `--strict=error` could promote all warnings to errors for strict CI.

## Files

| File | Change |
|------|--------|
| `include/pup/cli/strict_checks.hpp` | New: Diagnostic struct, check functions |
| `src/cli/strict_checks.cpp` | New: check function implementations |
| `include/pup/parser/eval.hpp` | Add `on_statement` callback to EvalContext |
| `src/cli/cmd_parse.cpp` | Wire --strict flag, callbacks, print diagnostics |
| `include/pup/cli/options.hpp` | Add `bool strict` to parse options |
| `test/unit/test_strict_checks.cpp` | New: unit tests for check functions |

## Testing

Unit tests (pure function tests, no E2E):
- Assignment `S = $(TUP_CWD)` in component → Error
- Assignment `S ?= $(TUP_CWD)` in component → no diagnostic
- Assignment `S = $(TUP_CWD)` in root → no diagnostic (exempt)
- Assignment `CC = gcc` in component → Warning
- Assignment `CC ?= gcc` in component → no diagnostic
- Component dir without Tupfile.ini → Warning
- Component dir with Tupfile.ini → no diagnostic

E2E test: create a fixture that violates conventions, run `putup parse --strict`, verify exit code and stderr output.

Positive E2E test: `examples/bsp/gcc/` should pass `--strict` (after adding missing `Tupfile.ini` to sub-library dirs if needed).

## What this does NOT do

- Does not modify parsing, evaluation, or graph building
- Does not block builds -- only `parse --strict` runs checks
- Does not check rule command expressions (Phase 3, future)
- Does not auto-fix violations
