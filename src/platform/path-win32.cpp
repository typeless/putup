// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/platform/path.hpp"

namespace pup::platform {

auto to_utf8(std::filesystem::path const& path) -> std::string
{
    auto u8 = path.u8string();
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    return std::string { reinterpret_cast<char const*>(u8.data()), u8.size() };
}

} // namespace pup::platform
