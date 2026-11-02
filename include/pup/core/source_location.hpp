// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include "pup/core/string_id.hpp"

#include <cstdint>
#include <optional>
#include <string_view>

namespace pup {

/// Source location for error reporting
struct SourceLocation {
    std::string_view filename;
    std::uint32_t line = 1;
    std::uint32_t column = 1;
    std::uint32_t offset = 0;
};

/// A stored position in a Tupfile or included file: what a message points at
struct SourcePosition {
    StringId file = StringId::Empty;
    std::uint32_t line = 0;
    std::optional<std::uint32_t> column = {};

    auto operator==(SourcePosition const& other) const -> bool = default;
};

} // namespace pup
