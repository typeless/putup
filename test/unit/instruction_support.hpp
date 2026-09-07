// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include "pup/core/buf.hpp"
#include "pup/core/instruction.hpp"
#include "pup/core/result.hpp"

#include <charconv>
#include <cstddef>
#include <string_view>
#include <utility>

namespace pup::test {
namespace {

auto kind_for_letter(char letter, bool numbered) -> pup::AtomKind
{
    if (numbered) {
        switch (letter) {
        case 'f':
            return pup::AtomKind::NthInput;
        case 'b':
            return pup::AtomKind::NthInputBase;
        case 'B':
            return pup::AtomKind::NthInputNoExt;
        default:
            return pup::AtomKind::NthOutput;
        }
    }
    switch (letter) {
    case 'f':
        return pup::AtomKind::AllInputs;
    case 'i':
        return pup::AtomKind::AllInputsAlias;
    case 'b':
        return pup::AtomKind::InputBase;
    case 'B':
        return pup::AtomKind::InputNoExt;
    case 'e':
        return pup::AtomKind::InputExt;
    case 'o':
        return pup::AtomKind::AllOutputs;
    case 'O':
        return pup::AtomKind::OutputNoExt;
    default:
        return pup::AtomKind::InputDir;
    }
}

auto is_unnumbered_flag(char letter) -> bool
{
    return letter == 'f' || letter == 'i' || letter == 'b' || letter == 'B' || letter == 'e'
        || letter == 'o' || letter == 'O' || letter == 'd';
}

} // namespace

inline auto parse_instruction_text(std::string_view text) -> pup::Result<pup::Instruction>
{
    auto builder = pup::InstructionBuilder {};
    auto pos = std::size_t { 0 };

    while (pos < text.size()) {
        auto const percent = text.find('%', pos);
        if (percent == std::string_view::npos) {
            builder.literal(text.substr(pos));
            break;
        }
        builder.literal(text.substr(pos, percent - pos));

        if (percent + 1 >= text.size()) {
            builder.literal('%');
            pos = percent + 1;
            continue;
        }

        auto const flag = text[percent + 1];
        pos = percent + 2;

        if (flag == '%') {
            builder.literal('%');
            continue;
        }

        if (flag >= '0' && flag <= '9') {
            auto end = pos;
            while (end < text.size() && text[end] >= '0' && text[end] <= '9') {
                ++end;
            }
            auto num = 0;
            std::from_chars(text.data() + percent + 1, text.data() + end, num);
            if (end >= text.size()) {
                auto msg = pup::Buf {};
                msg.fmt("Unfinished %{}-flag at the end of the string '{}'", num, text);
                return pup::make_error<pup::Instruction>(pup::ErrorCode::ParseError, msg.view());
            }
            auto const letter = text[end];
            if (letter != 'f' && letter != 'b' && letter != 'B' && letter != 'o') {
                auto msg = pup::Buf {};
                msg.fmt("Expected 'f', 'b', 'B', 'o', or 'i' after number in %{}-flag, but got '{}'", num, letter);
                return pup::make_error<pup::Instruction>(pup::ErrorCode::ParseError, msg.view());
            }
            if (!builder.nth(kind_for_letter(letter, true), num)) {
                auto msg = pup::Buf {};
                msg.fmt("Expected number from 1-99 (base 10) for %-flag, but got {}", num);
                return pup::make_error<pup::Instruction>(pup::ErrorCode::ParseError, msg.view());
            }
            pos = end + 1;
            continue;
        }

        if (flag == '<') {
            auto const end = text.find('>', pos);
            if (end == std::string_view::npos) {
                builder.literal("%<");
                continue;
            }
            builder.group_ref(text.substr(pos, end - pos));
            pos = end + 1;
            continue;
        }

        if (is_unnumbered_flag(flag)) {
            builder.flag(kind_for_letter(flag, false));
            continue;
        }

        builder.literal('%');
        builder.literal(flag);
    }

    return builder.take();
}


inline auto instruction(std::string_view text) -> pup::Instruction
{
    auto atoms = parse_instruction_text(text);
    return atoms ? std::move(*atoms) : pup::Instruction {};
}

} // namespace pup::test
