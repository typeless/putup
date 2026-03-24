// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include "pup/core/string.hpp"

#include <string_view>

namespace pup::platform {

[[nodiscard]]
auto to_utf8(std::string_view path) -> String;

} // namespace pup::platform
