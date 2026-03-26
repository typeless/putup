// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/core/fmt.hpp"

namespace pup {

auto format_impl(std::string_view pattern, FormatArg const* args, std::size_t count) -> String
{
    auto result = String {};
    result.reserve(pattern.size());
    format_to(result, pattern, args, count);
    return result;
}

} // namespace pup
