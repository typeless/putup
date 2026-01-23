// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/core/terminal.hpp"

#if defined(_WIN32)
#    include <io.h>
#    include <windows.h>
#else
#    include <sys/ioctl.h>
#    include <unistd.h>
#endif

namespace pup {

auto is_tty(int fd) -> bool
{
#if defined(_WIN32)
    return _isatty(fd) != 0;
#else
    return isatty(fd) != 0;
#endif
}

auto stdout_is_tty() -> bool
{
#if defined(_WIN32)
    return is_tty(_fileno(stdout));
#else
    return is_tty(STDOUT_FILENO);
#endif
}

auto stderr_is_tty() -> bool
{
#if defined(_WIN32)
    return is_tty(_fileno(stderr));
#else
    return is_tty(STDERR_FILENO);
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
    auto ws = winsize {};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
        return ws.ws_col;
    }
    return default_width;
#endif
}

} // namespace pup
