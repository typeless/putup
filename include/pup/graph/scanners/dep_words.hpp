// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include "pup/core/buf.hpp"
#include "pup/core/vec.hpp"

#include <span>
#include <string_view>

namespace pup::graph::scanners {

/// True for compiler-launcher front-ends that precede the real driver.
[[nodiscard]]
auto is_compiler_wrapper(std::string_view name) -> bool;

/// True if the word would be expanded by the shell rather than taken literally.
/// Such a word is dropped from the scan rather than carried: the scan's own shell would
/// evaluate the substitution a second time, on results the compile never saw.
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

/// True for a word made only of whitespace, which carries nothing into the scan.
[[nodiscard]]
auto is_blank_word(std::string_view word) -> bool;

/// True for the shell words after which a scan may read no further flags: a redirection whose
/// target belongs to the program rather than to the scan.
[[nodiscard]]
auto is_flag_barrier(std::string_view word) -> bool;

/// True for a leading `NAME=VALUE` word the scan keeps in front of the compiler, so the scan
/// preprocesses under the environment the compile did. A value carrying shell syntax is not one:
/// `FOO=$(id)` would be evaluated a second time, and `FOO=1;` ends the assignment where putup's
/// whitespace split did not.
[[nodiscard]]
auto is_env_assignment_word(std::string_view word) -> bool;

/// True for an invocation a scan may step over: one that runs nothing, or one running a program
/// that only writes to its output stream, no word of which carries shell syntax putup's whitespace
/// split left unresolved. Such an invocation changes neither the directory nor the environment nor
/// any file a later compile reads, so it need not be reproduced.
[[nodiscard]]
auto is_scan_transparent(std::span<std::string_view const> invocation) -> bool;

/// A flag the scan command carries, and what its argument is.
struct ArgFlag {
    std::string_view spelling;
    SeparateArg kind;
};

/// The joined-form row leading `word` -- its argument is the tail -- or nullptr.
[[nodiscard]]
auto find_joined_flag(std::span<ArgFlag const> table, std::string_view word) -> ArgFlag const*;

/// The separate-form row spelled exactly `word` -- its argument is the next word -- or nullptr.
[[nodiscard]]
auto find_separate_flag(std::span<ArgFlag const> table, std::string_view word) -> ArgFlag const*;

/// Whether any of `spellings` leads `word`. Flags that carry no argument are matched this way.
[[nodiscard]]
auto leads_any(std::span<std::string_view const> spellings, std::string_view word) -> bool;

/// A scanner's flag tables. The scan carries every word of the compile except the hazards, so
/// these say which arguments to normalize and consume, and which words must never ride along.
struct FlagTables {
    std::span<ArgFlag const> joined;
    std::span<ArgFlag const> separate;
    std::span<std::string_view const> hazards;
};

/// True if the word names a C/C++/ObjC/assembly translation unit.
[[nodiscard]]
auto is_source_file(std::string_view word) -> bool;

/// Strip a directory prefix and a `.exe` suffix from a program word.
[[nodiscard]]
auto program_basename(std::string_view word) -> std::string_view;

} // namespace pup::graph::scanners
