// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Putup authors

#pragma once

#include <cstddef>

namespace tiled {

auto page_size() -> std::size_t;
auto reserve_at(void* address, std::size_t bytes) -> bool;
auto commit(void* address, std::size_t bytes) -> void;
auto decommit(void* address, std::size_t bytes) -> void;

[[noreturn]]
auto fatal(char const* message, std::size_t length) -> void;

} // namespace tiled
