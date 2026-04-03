// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/cli/strict_checks.hpp"
#include "pup/core/buf.hpp"
#include "pup/core/global_pool.hpp"
#include "pup/core/path.hpp"
#include "pup/core/string_pool.hpp"
#include "pup/core/vec.hpp"
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
