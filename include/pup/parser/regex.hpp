// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include "pup/core/result.hpp"
#include "pup/core/vec.hpp"

#include <array>
#include <cstdint>
#include <string_view>

namespace pup::parser {

/// Regular expression for `^` input exclusions: the PCRE subset of literals, `.`, bracket
/// classes, `\d \w \s` and their negations, `* + ?`, `|`, groups and the `^ $` anchors.
/// Any other PCRE syntax is a compile error rather than a different meaning.
class Regex final {
public:
    /// Compile a pattern, or fail naming the pattern, the offset and the reason
    [[nodiscard]]
    static auto compile(std::string_view pattern) -> Result<Regex>;

    /// Check whether the pattern matches anywhere in the subject
    [[nodiscard]]
    auto search(std::string_view subject) const -> bool;

private:
    using CharSet = std::array<std::uint64_t, 4>;

    struct Instruction {
        enum class Op : std::uint8_t {
            Char,
            Any,
            Class,
            Split,
            Jump,
            LineStart,
            LineEnd,
            Match,
        };
        Op op = Op::Match;
        char c = 0;
        std::int32_t x = 0;
        std::int32_t y = 0;
    };

    Vec<Instruction> program_;
    Vec<CharSet> classes_;
};

} // namespace pup::parser
