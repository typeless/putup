// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/platform/stream.hpp"
#include "pup/platform/sys.hpp"

#include <cerrno>
#include <cstddef>
#include <string_view>

namespace pup::platform {

auto write_to(Stream stream, std::string_view text) -> void
{
    auto const fd = stream == Stream::Err ? 2 : 1;
    auto off = std::size_t { 0 };
    while (off < text.size()) {
        auto n = sys::write(fd, text.data() + off, text.size() - off);
        if (n <= 0) {
            if (n == -EINTR) {
                continue;
            }
            break;
        }
        off += static_cast<std::size_t>(n);
    }
}

auto flush(Stream stream) -> void
{
    (void)stream; // POSIX writes are unbuffered raw syscalls — nothing to flush
}

} // namespace pup::platform
