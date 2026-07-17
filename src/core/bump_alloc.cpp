// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/core/bump_alloc.hpp"
#include "pup/platform/vm.hpp"

#include <cstdio>
#include <cstdlib>

namespace pup {

// Zero-initialized POD state rather than a Region member: operator new
// runs during static initialization and after static destruction, so the
// allocator must not depend on constructor or destructor ordering.
namespace {

// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)
char* base = nullptr;
std::size_t reserved = 0;
std::size_t committed = 0;
std::size_t used = 0;
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

constexpr auto INITIAL_RESERVE = std::size_t { 1 } << 32;
constexpr auto MIN_RESERVE = std::size_t { 1 } << 24;

auto reserve_backing() -> void
{
    namespace vm = platform::vm;
    for (auto want = INITIAL_RESERVE; want >= MIN_RESERVE; want /= 2) {
        if (auto* p = vm::reserve(want)) {
            base = static_cast<char*>(p);
            reserved = want;
            return;
        }
    }
    std::fputs("fatal: cannot reserve bump region\n", stderr);
    std::abort();
}

} // namespace

auto bump_alloc(std::size_t size, std::size_t align) -> void*
{
    namespace vm = platform::vm;

    if (!base) {
        reserve_backing();
    }

    auto const offset = (used + align - 1) & ~(align - 1);
    auto const end = offset + size;
    if (end > reserved) {
        std::fputs("fatal: bump region exhausted\n", stderr);
        std::abort();
    }
    if (end > committed) {
        auto const page = vm::page_size();
        auto const new_committed = (end + page - 1) & ~(page - 1);
        vm::commit(base + committed, new_committed - committed);
        committed = new_committed;
    }
    used = end;
    return base + offset;
}

auto bump_allocated_bytes() -> std::size_t
{
    return used;
}

} // namespace pup
