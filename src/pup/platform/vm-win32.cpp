// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "tiled/platform.hpp"

#include <cstdio>
#include <cstdlib>
#include <windows.h>

namespace {

auto round_up_to_page(std::size_t bytes) -> std::size_t
{
    auto const page = tiled::page_size();
    return (bytes + page - 1) & ~(page - 1);
}

[[noreturn]]
auto die(char const* what) -> void
{
    std::fprintf(stderr, "%s: error %lu\n", what, GetLastError());
    std::abort();
}

} // namespace

namespace tiled {

auto page_size() -> std::size_t
{
    static auto const page = [] {
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        return static_cast<std::size_t>(si.dwPageSize);
    }();
    return page;
}

auto reserve_at(void* address, std::size_t bytes) -> bool
{
    return ::VirtualAlloc(address, bytes, MEM_RESERVE, PAGE_NOACCESS) == address;
}

auto commit(void* address, std::size_t bytes) -> void
{
    if (::VirtualAlloc(address, round_up_to_page(bytes), MEM_COMMIT, PAGE_READWRITE) == nullptr) {
        die("fatal: vm commit");
    }
}

auto decommit(void* address, std::size_t bytes) -> void
{
    if (::VirtualFree(address, round_up_to_page(bytes), MEM_DECOMMIT) == 0) {
        die("fatal: vm decommit");
    }
}

auto fatal(char const* message, std::size_t length) -> void
{
    std::fwrite(message, 1, length, stderr);
    std::fputc('\n', stderr);
    std::abort();
}

} // namespace tiled
