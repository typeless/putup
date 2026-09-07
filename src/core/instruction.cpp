// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/core/instruction.hpp"

#include "pup/core/buf.hpp"
#include "pup/core/global_pool.hpp"
#include "pup/core/string_id.hpp"
#include "pup/core/string_pool.hpp"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace pup {

namespace {

auto is_nth(AtomKind kind) -> bool
{
    switch (kind) {
    case AtomKind::NthInput:
    case AtomKind::NthInputBase:
    case AtomKind::NthInputNoExt:
    case AtomKind::NthOutput:
        return true;
    case AtomKind::Literal:
    case AtomKind::GroupRef:
    case AtomKind::AllInputs:
    case AtomKind::AllInputsAlias:
    case AtomKind::InputBase:
    case AtomKind::InputNoExt:
    case AtomKind::InputExt:
    case AtomKind::AllOutputs:
    case AtomKind::OutputNoExt:
    case AtomKind::InputDir:
        return false;
    }
    return false;
}

auto letter_of(AtomKind kind) -> char
{
    switch (kind) {
    case AtomKind::AllInputs:
    case AtomKind::NthInput:
        return 'f';
    case AtomKind::AllInputsAlias:
        return 'i';
    case AtomKind::InputBase:
    case AtomKind::NthInputBase:
        return 'b';
    case AtomKind::InputNoExt:
    case AtomKind::NthInputNoExt:
        return 'B';
    case AtomKind::InputExt:
        return 'e';
    case AtomKind::AllOutputs:
    case AtomKind::NthOutput:
        return 'o';
    case AtomKind::OutputNoExt:
        return 'O';
    case AtomKind::InputDir:
        return 'd';
    case AtomKind::Literal:
    case AtomKind::GroupRef:
        return '\0';
    }
    return '\0';
}

auto kind_for_letter(char letter, bool numbered) -> AtomKind
{
    if (numbered) {
        switch (letter) {
        case 'f':
            return AtomKind::NthInput;
        case 'b':
            return AtomKind::NthInputBase;
        case 'B':
            return AtomKind::NthInputNoExt;
        default:
            return AtomKind::NthOutput;
        }
    }
    switch (letter) {
    case 'f':
        return AtomKind::AllInputs;
    case 'i':
        return AtomKind::AllInputsAlias;
    case 'b':
        return AtomKind::InputBase;
    case 'B':
        return AtomKind::InputNoExt;
    case 'e':
        return AtomKind::InputExt;
    case 'o':
        return AtomKind::AllOutputs;
    case 'O':
        return AtomKind::OutputNoExt;
    default:
        return AtomKind::InputDir;
    }
}

auto is_unnumbered_flag(char letter) -> bool
{
    return letter == 'f' || letter == 'i' || letter == 'b' || letter == 'B' || letter == 'e'
        || letter == 'o' || letter == 'O' || letter == 'd';
}

} // namespace

auto InstructionBuilder::flush() -> void
{
    if (m_pending.empty()) {
        return;
    }
    m_atoms.push_back(Atom::literal(m_pending.intern(global_pool())));
    m_pending.clear();
}

auto InstructionBuilder::literal(std::string_view text) -> void
{
    if (text.empty()) {
        return;
    }
    m_pending.append(text);
}

auto InstructionBuilder::literal(char c) -> void
{
    m_pending.append(c);
}

auto InstructionBuilder::flag(AtomKind kind) -> void
{
    flush();
    m_atoms.push_back(Atom::flag(kind));
}

auto InstructionBuilder::nth(AtomKind kind, int operand) -> bool
{
    if (!is_nth(kind) || operand < 1 || operand > 98) {
        return false;
    }
    flush();
    m_atoms.push_back(Atom::nth(kind, static_cast<std::uint8_t>(operand)));
    return true;
}

auto InstructionBuilder::group_ref(std::string_view name) -> void
{
    flush();
    m_atoms.push_back(Atom::group_ref(global_pool().intern(name)));
}

auto InstructionBuilder::take() -> Instruction
{
    flush();
    return std::move(m_atoms);
}

auto render_instruction(Instruction const& atoms) -> StringId
{
    auto buf = Buf {};
    for (auto const& atom : atoms) {
        switch (atom.kind()) {
        case AtomKind::Literal: {
            auto const text = global_pool().get(atom.text());
            for (auto const c : text) {
                if (c == '%') {
                    buf.append('%');
                }
                buf.append(c);
            }
            break;
        }
        case AtomKind::GroupRef:
            buf.append("%<");
            buf.append(global_pool().get(atom.text()));
            buf.append('>');
            break;
        case AtomKind::AllInputs:
        case AtomKind::AllInputsAlias:
        case AtomKind::InputBase:
        case AtomKind::InputNoExt:
        case AtomKind::InputExt:
        case AtomKind::AllOutputs:
        case AtomKind::OutputNoExt:
        case AtomKind::InputDir:
        case AtomKind::NthInput:
        case AtomKind::NthInputBase:
        case AtomKind::NthInputNoExt:
        case AtomKind::NthOutput:
            buf.append('%');
            if (is_nth(atom.kind())) {
                auto digits = Buf {};
                digits.fmt("{}", static_cast<int>(atom.operand()));
                buf.append(digits.view());
            }
            buf.append(letter_of(atom.kind()));
            break;
        }
    }
    return buf.intern(global_pool());
}

auto parse_instruction(std::string_view text) -> Result<Instruction>
{
    auto builder = InstructionBuilder {};
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
                auto msg = Buf {};
                msg.fmt("Unfinished %{}-flag at the end of the string '{}'", num, text);
                return make_error<Instruction>(ErrorCode::ParseError, msg.view());
            }
            auto const letter = text[end];
            if (letter != 'f' && letter != 'b' && letter != 'B' && letter != 'o') {
                auto msg = Buf {};
                msg.fmt("Expected 'f', 'b', 'B', 'o', or 'i' after number in %{}-flag, but got '{}'", num, letter);
                return make_error<Instruction>(ErrorCode::ParseError, msg.view());
            }
            if (!builder.nth(kind_for_letter(letter, true), num)) {
                auto msg = Buf {};
                msg.fmt("Expected number from 1-99 (base 10) for %-flag, but got {}", num);
                return make_error<Instruction>(ErrorCode::ParseError, msg.view());
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

} // namespace pup
