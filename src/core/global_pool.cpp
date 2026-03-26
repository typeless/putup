// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/core/global_pool.hpp"
#include "pup/core/string_pool.hpp"

namespace pup {

auto global_pool() -> StringPool&
{
    thread_local StringPool pool;
    return pool;
}

} // namespace pup
