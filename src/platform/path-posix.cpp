// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/platform/path.hpp"

namespace pup::platform {

auto to_utf8(std::string_view path) -> String
{
    return String { path };
}

} // namespace pup::platform
