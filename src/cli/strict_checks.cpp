// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/cli/strict_checks.hpp"
#include "pup/cli/context.hpp"
#include "pup/core/buf.hpp"
#include "pup/core/global_pool.hpp"
#include "pup/core/path.hpp"
#include "pup/core/string_pool.hpp"
#include "pup/core/vec.hpp"
#include "pup/graph/dep_scanner.hpp"
#include "pup/parser/ast.hpp"
#include "pup/platform/file_io.hpp"

#include <cstddef>
#include <string_view>
#include <variant>

namespace pup::cli {

namespace {

auto is_tuprules(std::string_view file) -> bool
{
    return pup::path::filename(file) == "Tuprules.tup";
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

auto is_toolchain_var(std::string_view name) -> bool
{
    return name == "CC" || name == "CXX" || name == "AR" || name == "LD"
        || name == "AS" || name == "HOSTCC" || name == "HOSTCXX"
        || name == "RANLIB" || name == "STRIP" || name == "OBJCOPY" || name == "NM";
}

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

    if (!is_component || !is_tuprules(file)) {
        return result;
    }

    auto name_sv = stmt.name.as_literal();
    if (name_sv.empty()) {
        return result;
    }

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

    if (is_toolchain_var(name_sv) && stmt.op == parser::Assignment::Op::Set) {
        auto buf = Buf {};
        buf.fmt("'{}' should use '?=' in component Tuprules.tup. "
                "Using '=' overrides the parent project's toolchain choice.",
                name_sv);
        result.push_back(make_diag(file, stmt.location.line, Diagnostic::Warning, buf.view()));
    }

    return result;
}

auto check_unscanned_compiles(
    graph::Graph const& graph,
    graph::PathCache& cache
) -> Vec<Diagnostic>
{
    auto result = Vec<Diagnostic> {};
    auto registry = make_scanner_registry();
    if (!registry || registry->empty()) {
        return result;
    }

    auto& pool = global_pool();

    for (auto id : graph::nodes_of_type(graph, NodeType::Command)) {
        if (graph::get_parent_command(graph, id) != INVALID_NODE_ID) {
            continue;
        }

        auto object = std::string_view {};
        for (auto out_id : graph::get_outputs(graph, id)) {
            auto sv = pool.get(graph::get_full_path(graph, out_id));
            if (auto ext = pup::path::extension(sv); ext == ".o" || ext == ".obj") {
                object = sv;
                break;
            }
        }
        if (object.empty()) {
            continue;
        }

        auto text = graph::expand_instruction(graph, id, cache);
        auto source_dir = graph::get<graph::SourceDir>(graph, id);

        // The build reads a depfile from beside the object whether or not a scan exists,
        // so a compile that writes its own is covered without one.
        if (registry->reports_own_deps(pool.get(text))) {
            continue;
        }

        // The production predicate itself, not a re-derivation of it: whatever the build
        // would generate here is what decides whether this rule's headers get recorded.
        auto cmd_info = graph::CommandInfo {};
        cmd_info.node_id = id;
        cmd_info.command = text;
        cmd_info.working_dir = source_dir;
        if (!registry->match_and_generate(cmd_info).empty()) {
            continue;
        }

        auto dir = pool.get(source_dir);
        auto buf = Buf {};
        buf.fmt("no dependency scan for '{}': putup recognizes no compile it can reproduce in "
                "this command, so any header this rule reads goes unrecorded. Harmless if it "
                "preprocesses none — an assembler, a copy, a partial link.",
                object);
        result.push_back(make_diag(
            pool.get(pup::path::join(dir.empty() ? "." : dir, "Tupfile")),
            0,
            Diagnostic::Warning,
            buf.view()
        ));
    }

    return result;
}

auto check_component_dirs(
    Vec<std::string_view> const& component_dirs
) -> Vec<Diagnostic>
{
    auto result = Vec<Diagnostic> {};
    auto& pool = global_pool();

    for (auto dir : component_dirs) {
        auto ini_path = pool.get(pup::path::join(dir, "Tupfile.ini"));
        if (!pup::platform::exists(ini_path)) {
            auto buf = Buf {};
            buf.fmt("Component directory '{}' has no Tupfile.ini — cannot be built standalone.", dir);
            result.push_back(make_diag(dir, 0, Diagnostic::Warning, buf.view()));
        }
    }

    return result;
}

} // namespace pup::cli
