// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include "pup/core/buf.hpp"
#include "pup/core/global_pool.hpp"
#include "pup/core/string_id.hpp"
#include "pup/core/vec.hpp"

#include <cstdint>
#include <string_view>

namespace pup {

enum class AtomKind : std::uint8_t {
    Literal,
    GroupRef,
    AllInputs,
    AllInputsAlias,
    InputBase,
    InputNoExt,
    InputExt,
    AllOutputs,
    OutputNoExt,
    InputDir,
    NthInput,
    NthInputBase,
    NthInputNoExt,
    NthOutput,
};

class Atom final {
public:
    static auto literal(StringId text) -> Atom { return Atom { AtomKind::Literal, 0, text }; }

    [[nodiscard]]
    auto kind() const -> AtomKind
    {
        return m_kind;
    }
    [[nodiscard]]
    auto text() const -> StringId
    {
        return m_text;
    }
    [[nodiscard]]
    auto operand() const -> std::uint8_t
    {
        return m_operand;
    }

private:
    static auto flag(AtomKind kind) -> Atom { return Atom { kind, 0, StringId::Empty }; }

    static auto nth(AtomKind kind, std::uint8_t operand) -> Atom
    {
        return Atom { kind, operand, StringId::Empty };
    }

    static auto group_ref(StringId name) -> Atom { return Atom { AtomKind::GroupRef, 0, name }; }

    friend class InstructionBuilder;

    Atom(AtomKind kind, std::uint8_t operand, StringId text)
        : m_kind { kind }
        , m_operand { operand }
        , m_text { text }
    {
    }

    AtomKind m_kind;
    std::uint8_t m_operand;
    StringId m_text;
};

using Instruction = Vec<Atom>;

/// The only mint for a flag atom: the arity a numbered flag is allowed lives here, so an
/// atom carrying an operand the funnel would refuse cannot be constructed at all.
/// Adjacent literal text coalesces into one atom. A group name is taken up to its
/// closing '>', so it never spells one; render_instruction relies on that to round-trip.
class InstructionBuilder final {
public:
    auto literal(std::string_view text) -> void;
    auto literal(char c) -> void;
    auto flag(AtomKind kind) -> void;
    auto nth(AtomKind kind, int operand) -> bool;
    auto group_ref(std::string_view name) -> void;
    auto take() -> Instruction;

private:
    auto flush() -> void;

    Buf m_pending {};
    Instruction m_atoms {};
};

/// Renders atoms back to the v24 template spelling. A literal's own '%' is escaped,
/// so rendered text re-parses to the same atoms.
auto render_instruction(Instruction const& atoms) -> StringId;

template<typename Site>
auto fold_instruction(Instruction const& atoms, Site const& site) -> StringId
{
    auto buf = Buf {};
    for (auto const& atom : atoms) {
        switch (atom.kind()) {
        case AtomKind::Literal:
            site.append_literal(buf, atom.text());
            break;
        case AtomKind::GroupRef:
            site.append_group_ref(buf, atom.text());
            break;
        case AtomKind::AllInputs:
        case AtomKind::AllInputsAlias:
            site.append_all_inputs(buf);
            break;
        case AtomKind::InputBase:
            site.append_input_base(buf);
            break;
        case AtomKind::InputNoExt:
            site.append_input_noext(buf);
            break;
        case AtomKind::InputExt:
            site.append_input_ext(buf);
            break;
        case AtomKind::AllOutputs:
            site.append_all_outputs(buf);
            break;
        case AtomKind::OutputNoExt:
            site.append_output_noext(buf);
            break;
        case AtomKind::InputDir:
            site.append_input_dir(buf);
            break;
        case AtomKind::NthInput:
            site.append_nth_input(buf, atom.operand() - 1U);
            break;
        case AtomKind::NthInputBase:
            site.append_nth_input_base(buf, atom.operand() - 1U);
            break;
        case AtomKind::NthInputNoExt:
            site.append_nth_input_noext(buf, atom.operand() - 1U);
            break;
        case AtomKind::NthOutput:
            site.append_nth_output(buf, atom.operand() - 1U);
            break;
        }
    }
    return buf.intern(global_pool());
}

} // namespace pup
