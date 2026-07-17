// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include <cstddef>

namespace pup {

// Process-wide bump allocator backing the global operator new. Memory is
// never reused: operator delete is a no-op and the region dies with the
// process (batch lifetime). Safe to call before main.
auto bump_alloc(std::size_t size, std::size_t align) -> void*;

auto bump_allocated_bytes() -> std::size_t;

} // namespace pup
