// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/parser/ast.hpp"

#include "pup/core/global_pool.hpp"
#include "pup/core/string_pool.hpp"
#include <string_view>

namespace pup::parser {

auto Expression::as_literal() const -> std::string_view
{
    if (is_literal()) {
        return global_pool().get(std::get<Literal>(parts[0]).value);
    }
    return {};
}

// Conditional must be defined in .cpp because it uses unique_ptr with forward-declared Statement
Conditional::~Conditional() = default;
Conditional::Conditional() = default;
Conditional::Conditional(Conditional&&) noexcept = default;
auto Conditional::operator=(Conditional&&) noexcept -> Conditional& = default;

} // namespace pup::parser
