// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include "pup/core/string.hpp"
#include "pup/core/vec.hpp"

#include <string_view>

namespace pup::core {

/// Tokenize shell command into arguments with POSIX quoting semantics.
/// - Single quotes: no escaping, everything literal until closing '
/// - Double quotes: \\ and \" are special, other \X is literal
/// - Outside quotes: \ escapes next character
auto tokenize_shell_command(std::string_view cmd) -> Vec<String>;

} // namespace pup::core
