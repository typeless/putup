// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/platform/vm.hpp"

#include <cstdio>
#include <cstdlib>
#include <windows.h>

namespace pup::platform::vm {

namespace {

auto round_up_to_page(std::size_t bytes) -> std::size_t
{
    auto const page = page_size();
    return (bytes + page - 1) & ~(page - 1);
}

[[noreturn]]
auto die(char const* what) -> void
{
    std::fprintf(stderr, "%s: error %lu\n", what, GetLastError());
    std::abort();
}

} // namespace

auto page_size() -> std::size_t
{
    static auto const page = [] {
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        return static_cast<std::size_t>(si.dwPageSize);
    }();
    return page;
}

auto reserve(std::size_t bytes) -> void*
{
    return ::VirtualAlloc(nullptr, round_up_to_page(bytes), MEM_RESERVE, PAGE_NOACCESS);
}

auto commit(void* addr, std::size_t bytes) -> void
{
    if (::VirtualAlloc(addr, round_up_to_page(bytes), MEM_COMMIT, PAGE_READWRITE) == nullptr) {
        die("fatal: vm commit");
    }
}

auto decommit(void* addr, std::size_t bytes) -> void
{
    if (::VirtualFree(addr, round_up_to_page(bytes), MEM_DECOMMIT) == 0) {
        die("fatal: vm decommit");
    }
}

auto release(void* base, std::size_t /*bytes*/) -> void
{
    // MEM_RELEASE requires the reservation base and size 0; subrange
    // release is impossible on Win32 — hence the whole-reservation API.
    if (::VirtualFree(base, 0, MEM_RELEASE) == 0) {
        die("fatal: vm release");
    }
}

} // namespace pup::platform::vm
