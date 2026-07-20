// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include "pup/core/string_id.hpp"
#include "pup/core/vec.hpp"

#include <string_view>

namespace pup::core {

/// ASCII whitespace, locale-independent (the program never calls setlocale,
/// so this matches the C-locale isspace it replaces byte-for-byte).
constexpr auto is_space(char c) -> bool
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

/// Tokenize shell command into arguments with POSIX quoting semantics.
/// - Single quotes: no escaping, everything literal until closing '
/// - Double quotes: \\ and \" are special, other \X is literal
/// - Outside quotes: \ escapes next character
auto tokenize_shell_command(std::string_view cmd) -> Vec<StringId>;

} // namespace pup::core
