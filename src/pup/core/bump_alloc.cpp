// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/core/bump_alloc.hpp"
#include "pup/core/print.hpp"
#include "pup/platform/env.hpp"
#include "pup/platform/vm.hpp"

#include <charconv>
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
    write_to(Stream::Err, "fatal: cannot reserve bump region\n");
    std::abort();
}

} // namespace

// PROTOTYPE DIAGNOSTIC: per-call-site byte attribution, dumped at exit.
namespace {
struct SiteStat {
    void* ra;
    std::size_t bytes;
    std::size_t calls;
};
SiteStat site_stats[4096];    // NOLINT
bool dump_registered = false; // NOLINT

auto write_uint(std::size_t n) -> void
{
    char buf[20];
    auto [end, ec] = std::to_chars(buf, buf + sizeof(buf), n);
    write_to(Stream::Err, { buf, static_cast<std::size_t>(end - buf) });
}

auto write_ptr(void const* p) -> void
{
    char buf[18] = { '0', 'x' };
    auto [end, ec] = std::to_chars(buf + 2, buf + sizeof(buf), reinterpret_cast<std::uintptr_t>(p), 16);
    write_to(Stream::Err, { buf, static_cast<std::size_t>(end - buf) });
}

auto record_site(void* ra, std::size_t size) -> void
{
    auto h = (reinterpret_cast<std::size_t>(ra) >> 4) & 4095;
    while (site_stats[h].ra && site_stats[h].ra != ra) {
        h = (h + 1) & 4095;
    }
    site_stats[h].ra = ra;
    site_stats[h].bytes += size;
    site_stats[h].calls += 1;
    if (!dump_registered) {
        dump_registered = true;
        std::atexit([] {
            auto gross = std::size_t { 0 };
            for (auto const& s : site_stats) {
                gross += s.bytes;
                if (s.bytes > 1U << 20) {
                    write_to(Stream::Err, "bumpsite ");
                    write_ptr(s.ra);
                    write_to(Stream::Err, " ");
                    write_uint(s.bytes);
                    write_to(Stream::Err, " bytes ");
                    write_uint(s.calls);
                    write_to(Stream::Err, " calls\n");
                }
            }
            write_to(Stream::Err, "bumptotal gross ");
            write_uint(gross);
            write_to(Stream::Err, " net ");
            write_uint(used);
            write_to(Stream::Err, "\n");
        });
    }
}
} // namespace

auto bump_alloc(std::size_t size, std::size_t align) -> void*
{
    static bool const stats_enabled = pup::platform::get_env("PUP_BUMP_STATS") != nullptr;
    if (stats_enabled) {
        record_site(__builtin_return_address(0), size);
    }
    namespace vm = platform::vm;

    if (!base) {
        reserve_backing();
    }

    auto const offset = (used + align - 1) & ~(align - 1);
    auto const end = offset + size;
    if (end > reserved) {
        write_to(Stream::Err, "fatal: bump region exhausted\n");
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

auto bump_try_extend(void* p, std::size_t old_size, std::size_t new_size) -> bool
{
    if (static_cast<char*>(p) + old_size != base + used) {
        return false;
    }
    namespace vm = platform::vm;
    auto const end = static_cast<std::size_t>(static_cast<char*>(p) - base) + new_size;
    if (end > reserved) {
        return false;
    }
    if (end > committed) {
        auto const page = vm::page_size();
        auto const new_committed = (end + page - 1) & ~(page - 1);
        vm::commit(base + committed, new_committed - committed);
        committed = new_committed;
    }
    used = end;
    return true;
}

auto bump_try_pop(void* p, std::size_t size) -> void
{
    if (static_cast<char*>(p) + size == base + used) {
        used = static_cast<std::size_t>(static_cast<char*>(p) - base);
    }
}

auto bump_allocated_bytes() -> std::size_t
{
    return used;
}

} // namespace pup
