// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/core/memory_layout.hpp"
#include "pup/mem/bump_alloc.hpp"
#include "pup/platform/env.hpp"
#include "pup/platform/stream.hpp"
#include "tiled/platform.hpp"
#include "tiled/space_storage.hpp"
#include "tiled/stack.hpp"
#include "tiled/tile.hpp"

#include <bit>
#include <charconv>
#include <cstddef>
#include <cstdint>
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

namespace {

constexpr auto SPILL_BYTES = std::size_t { 1 } << 16;
constexpr auto HEAP_END = memory_layout.base + (std::uint64_t { 1 } << memory_layout.heap_log2);
constexpr auto POOL_END = memory_layout.base + (std::uint64_t { 1 } << memory_layout.pool_log2);

auto on_tile(void const* p) -> bool
{
    auto const address = reinterpret_cast<std::uintptr_t>(p);
    return address >= HEAP_END && address < POOL_END;
}

auto tile_log2_for(std::size_t bytes) -> std::uint8_t
{
    auto const wanted = static_cast<std::uint8_t>(std::bit_width(bytes - 1));
    return wanted > memory_layout.min_tile_log2 ? wanted : memory_layout.min_tile_log2;
}

auto round_up_to_page(std::size_t bytes) -> std::size_t
{
    auto const page = tiled::page_size();
    return (bytes + page - 1) & ~(page - 1);
}

auto acquire_tile(std::size_t size) -> void*
{
    auto const offset = process_pool().acquire(tile_log2_for(size));
    if (!offset) {
        platform::write_to(platform::Stream::Err, "fatal: the pool has no free tile for a spilled allocation\n");
        std::abort();
    }
    auto* const base = reinterpret_cast<char*>(memory_layout.base + *offset);
    tiled::commit(base, round_up_to_page(size));
    return base;
}

auto extend_tile(void* p, std::size_t old_size, std::size_t new_size) -> bool
{
    if (new_size > std::size_t { 1 } << tile_log2_for(old_size)) {
        return false;
    }
    auto const committed = round_up_to_page(old_size);
    auto const needed = round_up_to_page(new_size);
    if (needed > committed) {
        tiled::commit(static_cast<char*>(p) + committed, needed - committed);
    }
    return true;
}

auto release_tile(void* p, std::size_t size) -> void
{
    auto const log2 = tile_log2_for(size);
    tiled::decommit(p, std::size_t { 1 } << log2);
    if (!process_pool().release(reinterpret_cast<std::uintptr_t>(p) - memory_layout.base, log2)) {
        platform::write_to(platform::Stream::Err, "fatal: the pool refused the release of a spilled allocation\n");
        std::abort();
    }
}

} // namespace

auto bump_alloc(std::size_t size, std::size_t align) -> void*
{
    static bool const stats_enabled = pup::platform::get_env("PUP_BUMP_STATS") != nullptr;
    if (stats_enabled) {
        record_site(__builtin_return_address(0), size);
    }
    if (size >= SPILL_BYTES) {
        return acquire_tile(size);
    }
    return heap().push(size, align);
}

auto bump_try_extend(void* p, std::size_t old_size, std::size_t new_size) -> bool
{
    if (on_tile(p)) {
        return extend_tile(p, old_size, new_size);
    }
    return heap().extend_top(p, old_size, new_size);
}

auto bump_try_pop(void* p, std::size_t size) -> void
{
    if (on_tile(p)) {
        release_tile(p, size);
        return;
    }
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

auto process_pool() -> tiled::Pool&
{
    heap();
    return space.pool();
}

} // namespace pup::mem
