// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/platform/path.hpp"

namespace pup::platform {

auto to_utf8(std::filesystem::path const& path) -> std::string
{
    return path.string();
}

} // namespace pup::platform
