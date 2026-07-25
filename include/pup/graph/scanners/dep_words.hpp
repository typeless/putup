// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include "pup/core/buf.hpp"

#include <string_view>

namespace pup::graph::scanners {

/// True for compiler-launcher front-ends that precede the real driver.
[[nodiscard]]
auto is_compiler_wrapper(std::string_view name) -> bool;

/// True if the word would be expanded by the shell rather than taken literally.
[[nodiscard]]
auto has_shell_special(std::string_view flag) -> bool;

/// Append `s` to `out`, single-quoted if the shell would otherwise mangle it.
auto shell_quote_into(Buf& out, std::string_view s) -> void;

/// Append `path` to `out` with `.`/`..` segments resolved textually.
auto normalize_path_lexically_into(Buf& out, std::string_view path) -> void;

/// True if the word names a C/C++/ObjC/assembly translation unit.
[[nodiscard]]
auto is_source_file(std::string_view word) -> bool;

/// Strip a directory prefix and a `.exe` suffix from a program word.
[[nodiscard]]
auto program_basename(std::string_view word) -> std::string_view;

} // namespace pup::graph::scanners
