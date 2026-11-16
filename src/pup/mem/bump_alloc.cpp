// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/core/memory_layout.hpp"
#include "pup/mem/bump_alloc.hpp"
#include "pup/platform/env.hpp"
#include "pup/platform/stream.hpp"
#include "tiled/space_storage.hpp"
#include "tiled/stack.hpp"
#include "tiled/tile.hpp"

#include <charconv>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <new>
#include <type_traits>

namespace pup::mem {

// Zero-initialized POD state rather than a Region member: operator new
// runs during static initialization and after static destruction, so the
// allocator must not depend on constructor or destructor ordering.
namespace {

// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)
constinit tiled::SpaceStorage<memory_layout> space;
alignas(tiled::Stack) std::byte heap_storage[sizeof(tiled::Stack)];
tiled::Stack* heap_stack = nullptr;
std::size_t copied = 0;
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

static_assert(std::is_trivially_destructible_v<tiled::SpaceStorage<memory_layout>>);

auto heap() -> tiled::Stack&
{
    if (!heap_stack) {
        heap_stack = new (heap_storage) tiled::Stack { tiled::Tile::acquire(space.pool(), memory_layout.heap_log2) };
    }
    return *heap_stack;
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
    platform::write_to(platform::Stream::Err, { buf, static_cast<std::size_t>(end - buf) });
}

auto write_ptr(void const* p) -> void
{
    char buf[18] = { '0', 'x' };
    auto [end, ec] = std::to_chars(buf + 2, buf + sizeof(buf), reinterpret_cast<std::uintptr_t>(p), 16);
    platform::write_to(platform::Stream::Err, { buf, static_cast<std::size_t>(end - buf) });
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
            auto allocations = std::size_t { 0 };
            for (auto const& s : site_stats) {
                gross += s.bytes;
                allocations += s.calls;
                if (s.bytes > 1U << 20) {
                    platform::write_to(platform::Stream::Err, "bumpsite ");
                    write_ptr(s.ra);
                    platform::write_to(platform::Stream::Err, " ");
                    write_uint(s.bytes);
                    platform::write_to(platform::Stream::Err, " bytes ");
                    write_uint(s.calls);
                    platform::write_to(platform::Stream::Err, " calls\n");
                }
            }
            platform::write_to(platform::Stream::Err, "bumptotal gross ");
            write_uint(gross);
            platform::write_to(platform::Stream::Err, " net ");
            write_uint(bump_allocated_bytes());
            platform::write_to(platform::Stream::Err, " allocations ");
            write_uint(allocations);
            platform::write_to(platform::Stream::Err, "\n");
            platform::write_to(platform::Stream::Err, "copytotal ");
            write_uint(copied_bytes());
            platform::write_to(platform::Stream::Err, "\n");
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
    return heap().push(size, align);
}

auto bump_try_extend(void* p, std::size_t old_size, std::size_t new_size) -> bool
{
    return heap().extend_top(p, old_size, new_size);
}

auto bump_try_pop(void* p, std::size_t size) -> void
{
    heap().pop_top(p, size);
}

auto bump_allocated_bytes() -> std::size_t
{
    return heap().top();
}

auto relocate(void* target, void const* source, std::size_t bytes) -> void
{
    std::memcpy(target, source, bytes);
    copied += bytes;
}

auto record_copy(std::size_t bytes) -> void
{
    copied += bytes;
}

auto copied_bytes() -> std::size_t
{
    return copied;
}

} // namespace pup::mem
