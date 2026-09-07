// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include "pup/core/instruction.hpp"

#include <string_view>
#include <utility>

namespace pup::test {

inline auto instruction(std::string_view text) -> pup::Instruction
{
    auto atoms = pup::parse_instruction(text);
    return atoms ? std::move(*atoms) : pup::Instruction {};
}

} // namespace pup::test
