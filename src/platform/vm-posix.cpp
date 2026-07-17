// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/platform/vm.hpp"

#include <cstdio>
#include <cstdlib>
#include <sys/mman.h>
#include <unistd.h>

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
    std::perror(what);
    std::abort();
}

} // namespace

auto page_size() -> std::size_t
{
    static auto const page = static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
    return page;
}

auto reserve(std::size_t bytes) -> void*
{
    auto flags = MAP_PRIVATE | MAP_ANONYMOUS;
#ifdef MAP_NORESERVE
    flags |= MAP_NORESERVE;
#endif
    auto* p = ::mmap(nullptr, round_up_to_page(bytes), PROT_NONE, flags, -1, 0);
    return p == MAP_FAILED ? nullptr : p;
}

auto commit(void* addr, std::size_t bytes) -> void
{
    if (::mprotect(addr, round_up_to_page(bytes), PROT_READ | PROT_WRITE) != 0) {
        die("fatal: vm commit");
    }
}

auto decommit(void* addr, std::size_t bytes) -> void
{
    // Remap with a fresh PROT_NONE anonymous mapping rather than madvise:
    // frees the pages and guarantees zero-fill on recommit with identical
    // semantics on Linux and macOS (MADV_DONTNEED differs between them).
    auto* p = ::mmap(addr, round_up_to_page(bytes), PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (p == MAP_FAILED) {
        die("fatal: vm decommit");
    }
}

auto release(void* base, std::size_t bytes) -> void
{
    if (::munmap(base, round_up_to_page(bytes)) != 0) {
        die("fatal: vm release");
    }
}

} // namespace pup::platform::vm
