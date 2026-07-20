// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/core/terminal.hpp"

#if defined(_WIN32)
#    include <cstdio>
#    include <io.h>
#    include <windows.h>
#else
#    include "pup/platform/sys.hpp"
#endif

namespace pup {

auto is_tty(int fd) -> bool
{
#if defined(_WIN32)
    return _isatty(fd) != 0;
#else
    return platform::sys::isatty(fd);
#endif
}

auto stdout_is_tty() -> bool
{
#if defined(_MSC_VER)
    return is_tty(_fileno(stdout));
#elif defined(_WIN32)
    return is_tty(fileno(stdout));
#else
    return is_tty(1);
#endif
}

auto stderr_is_tty() -> bool
{
#if defined(_MSC_VER)
    return is_tty(_fileno(stderr));
#elif defined(_WIN32)
    return is_tty(fileno(stderr));
#else
    return is_tty(2);
#endif
}

auto terminal_width() -> int
{
    auto constexpr default_width = 80;

#if defined(_WIN32)
    auto csbi = CONSOLE_SCREEN_BUFFER_INFO {};
    auto handle = GetStdHandle(STD_OUTPUT_HANDLE);
    if (handle != INVALID_HANDLE_VALUE && GetConsoleScreenBufferInfo(handle, &csbi)) {
        return csbi.srWindow.Right - csbi.srWindow.Left + 1;
    }
    return default_width;
#else
    auto cols = platform::sys::terminal_width(1);
    return cols > 0 ? cols : default_width;
#endif
}

} // namespace pup
