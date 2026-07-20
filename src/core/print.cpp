// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/core/print.hpp"

#include "pup/core/buf.hpp"
#include "pup/core/format_to.hpp"

#include <cstddef>
#include <string_view>

#ifdef _WIN32
#    include <cstdio>
#else
#    include "pup/platform/sys.hpp"

#    include <cerrno>
#endif

namespace pup {

auto write_to(Stream stream, std::string_view text) -> void
{
#ifdef _WIN32
    std::fwrite(text.data(), 1, text.size(), stream == Stream::Err ? stderr : stdout);
#else
    auto const fd = stream == Stream::Err ? 2 : 1;
    auto off = std::size_t { 0 };
    while (off < text.size()) {
        auto n = platform::sys::write(fd, text.data() + off, text.size() - off);
        if (n <= 0) {
            if (n == -EINTR) {
                continue;
            }
            break;
        }
        off += static_cast<std::size_t>(n);
    }
#endif
}

auto flush(Stream stream) -> void
{
#ifdef _WIN32
    std::fflush(stream == Stream::Err ? stderr : stdout);
#else
    (void)stream; // POSIX writes are unbuffered raw syscalls — nothing to flush
#endif
}

auto print_args(Stream stream, std::string_view pattern, FormatArg const* args, std::size_t count) -> void
{
    auto buf = Buf {};
    format_to(buf, pattern, args, count);
    write_to(stream, buf.view());
}

} // namespace pup
