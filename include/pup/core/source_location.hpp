// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include <cstdint>
#include <string_view>

namespace pup {

/// Source location for error reporting
struct SourceLocation {
    std::string_view filename;
    std::uint32_t line = 1;
    std::uint32_t column = 1;
    std::uint32_t offset = 0;
};

} // namespace pup
