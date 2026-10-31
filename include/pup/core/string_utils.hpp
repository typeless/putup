// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include "pup/core/string_id.hpp"
#include "pup/core/vec.hpp"

#include <cstdint>
#include <string_view>

namespace pup::core {

/// ASCII whitespace, locale-independent (the program never calls setlocale,
/// so this matches the C-locale isspace it replaces byte-for-byte).
/// A trim/boundary predicate; word-splitting follows the shell, not this (see tokenize_shell_command).
constexpr auto is_space(char c) -> bool
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

/// What a shell token is: a word the program receives, or an unquoted control operator.
enum class ShellTokenKind : std::uint8_t {
    Word,
    Operator,
};

/// One token of a shell command, its quoting already removed.
struct ShellToken {
    StringId text;
    ShellTokenKind kind;
};

/// Tokenize shell command into arguments with POSIX quoting semantics.
/// Splits on space and tab — sh's blank class; see the definition for why.
/// An unquoted ;, &, &&, |, || or newline is an Operator token, except an & or | that follows < or >.
/// A quoted empty word, '' or "", is a Word with empty text.
/// An unquoted < or > begins a word unless the word so far is a file descriptor number.
/// - Single quotes: no escaping, everything literal until closing '
/// - Double quotes: \\ and \" are special, other \X is literal
/// - Outside quotes: \ escapes next character
auto tokenize_shell_command(std::string_view cmd) -> Vec<ShellToken>;

} // namespace pup::core
