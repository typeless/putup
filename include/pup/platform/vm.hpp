// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include <cstddef>

namespace pup::platform::vm {

auto page_size() -> std::size_t;

// Reserves address space without committing memory. Returns nullptr on
// failure so callers can probe-and-halve. The address is page-aligned;
// bytes are rounded up to page size.
auto reserve(std::size_t bytes) -> void*;

// Commits pages inside a reservation as zero-filled read-write memory.
// addr must be page-aligned. Aborts on failure (out of memory).
auto commit(void* addr, std::size_t bytes) -> void;

// Returns pages to the OS; a later commit of the same range reads as
// zero again. addr must be page-aligned.
auto decommit(void* addr, std::size_t bytes) -> void;

// Releases a whole reservation. base and bytes must match the reserve
// call exactly (Win32 cannot release subranges).
auto release(void* base, std::size_t bytes) -> void;

} // namespace pup::platform::vm
