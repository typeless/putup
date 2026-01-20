// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/parser/ast.hpp"

namespace pup::parser {

// Conditional must be defined in .cpp because it uses unique_ptr with forward-declared Statement
Conditional::~Conditional() = default;
Conditional::Conditional() = default;
Conditional::Conditional(Conditional&&) noexcept = default;
auto Conditional::operator=(Conditional&&) noexcept -> Conditional& = default;

} // namespace pup::parser
