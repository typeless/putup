// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/platform/vm.hpp"

#include "pup/platform/sys.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>

namespace pup::platform::vm {

namespace {

auto round_up_to_page(std::size_t bytes) -> std::size_t
{
    auto const page = page_size();
    return (bytes + page - 1) & ~(page - 1);
}

[[noreturn]]
auto die(char const* what, int rc) -> void
{
    errno = -rc;
    std::perror(what);
    std::abort();
}

} // namespace

auto page_size() -> std::size_t
{
    return sys::page_size();
}

auto reserve(std::size_t bytes) -> void*
{
    return sys::reserve(round_up_to_page(bytes));
}

auto commit(void* addr, std::size_t bytes) -> void
{
    auto rc = sys::commit(addr, round_up_to_page(bytes));
    if (rc != 0) {
        die("fatal: vm commit", rc);
    }
}

auto decommit(void* addr, std::size_t bytes) -> void
{
    // Remap with a fresh PROT_NONE anonymous mapping rather than madvise:
    // frees the pages and guarantees zero-fill on recommit with identical
    // semantics on Linux and macOS (MADV_DONTNEED differs between them).
    auto rc = sys::remap_none(addr, round_up_to_page(bytes));
    if (rc != 0) {
        die("fatal: vm decommit", rc);
    }
}

auto release(void* base, std::size_t bytes) -> void
{
    auto rc = sys::unmap(base, round_up_to_page(bytes));
    if (rc != 0) {
        die("fatal: vm release", rc);
    }
}

} // namespace pup::platform::vm
