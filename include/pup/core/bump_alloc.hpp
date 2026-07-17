// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include <cstddef>

namespace pup {

// Process-wide bump allocator backing the global operator new and the
// growable containers. There is no free list — the region dies with the
// process (batch lifetime) and operator delete is a no-op — but the top
// of the bump is reusable obstack-style: the most recent allocation can
// grow in place or be reclaimed, which is exactly the lifetime shape of
// transient container locals. Safe to call before main.
auto bump_alloc(std::size_t size, std::size_t align) -> void*;

// Grows the allocation at p in place if it is the top of the bump.
// Returns false (allocation untouched) otherwise.
auto bump_try_extend(void* p, std::size_t old_size, std::size_t new_size) -> bool;

// Reclaims the allocation at p if it is the top of the bump; no-op otherwise.
auto bump_try_pop(void* p, std::size_t size) -> void;

auto bump_allocated_bytes() -> std::size_t;

} // namespace pup
