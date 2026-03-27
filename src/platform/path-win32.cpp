// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/platform/path.hpp"
#include "pup/core/global_pool.hpp"
#include "pup/core/string_pool.hpp"

namespace pup::platform {

auto to_utf8(std::string_view path) -> StringId
{
    return global_pool().intern(path);
}

} // namespace pup::platform
