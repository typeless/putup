// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include <string_view>

namespace pup {

/// Check if file descriptor is a TTY
[[nodiscard]]
auto is_tty(int fd) -> bool;

/// Check if stdout is a TTY
[[nodiscard]]
auto stdout_is_tty() -> bool;

/// Check if stderr is a TTY
[[nodiscard]]
auto stderr_is_tty() -> bool;

/// Get terminal width (or default if not TTY)
[[nodiscard]]
auto terminal_width() -> int;

/// ANSI escape sequences
namespace ansi {

inline constexpr auto clear_line = std::string_view { "\033[K" };
inline constexpr auto move_up = std::string_view { "\033[A" };
inline constexpr auto carriage_return = std::string_view { "\r" };

} // namespace ansi

} // namespace pup
