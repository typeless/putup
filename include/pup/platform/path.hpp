// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include <string>

namespace pup::platform {

[[nodiscard]]
auto to_utf8(std::string const& path) -> std::string;

} // namespace pup::platform
