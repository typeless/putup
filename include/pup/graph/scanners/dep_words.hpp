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

/// The shell a scan command will be handed to: putup spawns commands through
/// `sh -c` on POSIX and `cmd.exe /c` on Windows, which share no quoting syntax.
enum class QuoteStyle {
    Posix,
    Windows,
};

inline constexpr auto host_quote_style =
#ifdef _WIN32
    QuoteStyle::Windows;
#else
    QuoteStyle::Posix;
#endif

/// Append `s` to `out`, quoted if `style`'s shell would otherwise mangle it.
auto shell_quote_into(Buf& out, std::string_view s, QuoteStyle style = host_quote_style) -> void;

/// What the word after a flag is, for the flags that take a separate one.
enum class SeparateArg {
    Path,  ///< an include or sysroot path, normalized before it enters the scan
    Value, ///< a macro name or definition, which is the preprocessor's and not a path
};

/// Append `word` to `out` as `kind` says, preceded by a space and quoted for the scan's shell.
auto append_separate_arg_into(Buf& out, std::string_view word, SeparateArg kind) -> void;

/// True if the word names a C/C++/ObjC/assembly translation unit.
[[nodiscard]]
auto is_source_file(std::string_view word) -> bool;

/// Strip a directory prefix and a `.exe` suffix from a program word.
[[nodiscard]]
auto program_basename(std::string_view word) -> std::string_view;

} // namespace pup::graph::scanners
