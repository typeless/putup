// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include <functional>
#include <string>
#include <string_view>

namespace pup {

/// Transparent hash for heterogeneous lookup in unordered containers.
/// Enables find(string_view) on unordered_map<string, V>.
struct StringHash {
    using is_transparent = void;

    auto operator()(std::string_view sv) const noexcept -> std::size_t
    {
        return std::hash<std::string_view> {}(sv);
    }

    auto operator()(std::string const& s) const noexcept -> std::size_t
    {
        return std::hash<std::string_view> {}(s);
    }
};

} // namespace pup
