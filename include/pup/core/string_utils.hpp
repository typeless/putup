// SPDX-License-Identifier: MIT
// Copyright (c) 2024 pup authors

#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace pup::core {

/// Tokenize shell command into arguments with POSIX quoting semantics.
/// - Single quotes: no escaping, everything literal until closing '
/// - Double quotes: \\ and \" are special, other \X is literal
/// - Outside quotes: \ escapes next character
auto tokenize_shell_command(std::string_view cmd) -> std::vector<std::string>;

} // namespace pup::core
