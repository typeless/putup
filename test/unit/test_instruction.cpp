// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "catch_amalgamated.hpp"
#include "pup/core/global_pool.hpp"
#include "instruction_support.hpp"
#include "pup/core/instruction.hpp"
#include "pup/core/string_pool.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

using namespace pup;

namespace {

class Lcg {
public:
    explicit Lcg(std::uint64_t seed)
        : state_ { seed }
    {
    }

    auto next(std::uint32_t bound) -> std::uint32_t
    {
        state_ = state_ * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<std::uint32_t>(state_ >> 33) % bound;
    }

private:
    std::uint64_t state_;
};

struct RecordingSite {
    std::vector<std::string>* log;

    auto append_literal(Buf& buf, StringId text) const -> void { buf.append(global_pool().get(text)); }
    auto append_group_ref(Buf& buf, StringId name) const -> void
    {
        buf.append("<");
        buf.append(global_pool().get(name));
        buf.append(">");
    }
    auto append_all_inputs(Buf& buf) const -> void { note(buf, "all_inputs"); }
    auto append_input_base(Buf& buf) const -> void { note(buf, "input_base"); }
    auto append_input_noext(Buf& buf) const -> void { note(buf, "input_noext"); }
    auto append_input_ext(Buf& buf) const -> void { note(buf, "input_ext"); }
    auto append_all_outputs(Buf& buf) const -> void { note(buf, "all_outputs"); }
    auto append_output_noext(Buf& buf) const -> void { note(buf, "output_noext"); }
    auto append_input_dir(Buf& buf) const -> void { note(buf, "input_dir"); }
    auto append_nth_input(Buf& buf, std::size_t i) const -> void { note_nth(buf, "nth_input", i); }
    auto append_nth_input_base(Buf& buf, std::size_t i) const -> void { note_nth(buf, "nth_input_base", i); }
    auto append_nth_input_noext(Buf& buf, std::size_t i) const -> void { note_nth(buf, "nth_input_noext", i); }
    auto append_nth_output(Buf& buf, std::size_t i) const -> void { note_nth(buf, "nth_output", i); }

private:
    auto note(Buf& buf, std::string_view what) const -> void
    {
        log->emplace_back(what);
        buf.append("{");
        buf.append(what);
        buf.append("}");
    }

    auto note_nth(Buf& buf, std::string_view what, std::size_t i) const -> void
    {
        auto entry = std::string { what } + ":" + std::to_string(i);
        log->push_back(entry);
        buf.append("{");
        buf.append(entry);
        buf.append("}");
    }
};

auto atoms_equal(Instruction const& a, Instruction const& b) -> bool
{
    if (a.size() != b.size()) {
        return false;
    }
    for (auto i = std::size_t { 0 }; i < a.size(); ++i) {
        if (a[i].kind() != b[i].kind() || a[i].operand() != b[i].operand() || a[i].text() != b[i].text()) {
            return false;
        }
    }
    return true;
}

auto describe(Instruction const& atoms) -> std::string
{
    auto out = std::string {};
    for (auto const& atom : atoms) {
        out += "[" + std::to_string(static_cast<int>(atom.kind())) + ","
            + std::to_string(static_cast<int>(atom.operand())) + ","
            + std::string { global_pool().get(atom.text()) } + "]";
    }
    return out;
}

} // namespace

SCENARIO("Text appended as a literal never expands", "[instruction]")
{
    GIVEN("an instruction that is one literal carrying every flag spelling")
    {
        auto builder = InstructionBuilder {};
        builder.literal("%1f %g %<gen> %0f %% %f");
        auto const atoms = builder.take();

        WHEN("it is folded at a site that expands flags")
        {
            auto log = std::vector<std::string> {};
            auto const folded = fold_instruction(atoms, RecordingSite { &log });

            THEN("the bytes come through unchanged and no flag is resolved")
            {
                INFO("folded: " << global_pool().get(folded));
                REQUIRE(global_pool().get(folded) == "%1f %g %<gen> %0f %% %f");
                REQUIRE(log.empty());
            }
        }
    }
}

SCENARIO("An operand outside the range the funnel accepts builds no atom", "[instruction]")
{
    GIVEN("a builder")
    {
        WHEN("a numbered flag is built at each boundary")
        {
            auto builder = InstructionBuilder {};

            THEN("1 and 98 are accepted and 0 and 99 are refused")
            {
                REQUIRE(builder.nth(AtomKind::NthInput, 1));
                REQUIRE(builder.nth(AtomKind::NthOutput, 98));
                REQUIRE_FALSE(builder.nth(AtomKind::NthInput, 0));
                REQUIRE_FALSE(builder.nth(AtomKind::NthInput, 99));
                REQUIRE_FALSE(builder.nth(AtomKind::NthInput, -1));
                REQUIRE_FALSE(builder.nth(AtomKind::AllInputs, 1));
            }
        }
    }
}

SCENARIO("A rendered instruction re-parses to the same atoms", "[instruction][property]")
{
    GIVEN("instructions built through the funnel's own vocabulary")
    {
        auto const seed = GENERATE(1U, 2U, 3U, 12345U, 999983U);
        auto rng = Lcg { seed };

        auto const literals = std::vector<std::string_view> {
            "gcc -c ", " -o ", "%", "%%", "a%1fb.c", "echo ", " > ", "", "üñ", "%<not-a-group",
        };
        auto const plain = std::vector<AtomKind> {
            AtomKind::AllInputs, AtomKind::AllInputsAlias, AtomKind::InputBase,
            AtomKind::InputNoExt, AtomKind::InputExt, AtomKind::AllOutputs,
            AtomKind::OutputNoExt, AtomKind::InputDir,
        };
        auto const numbered = std::vector<AtomKind> {
            AtomKind::NthInput, AtomKind::NthInputBase, AtomKind::NthInputNoExt, AtomKind::NthOutput,
        };
        auto const groups = std::vector<std::string_view> { "gen", "gen-headers", "objs" };

        WHEN("each is rendered and parsed back")
        {
            auto failures = 0;
            auto first_failure = std::string {};

            for (auto trial = 0; trial < 512; ++trial) {
                auto builder = InstructionBuilder {};
                auto const parts = 1U + rng.next(6);
                for (auto p = 0U; p < parts; ++p) {
                    switch (rng.next(4)) {
                    case 0:
                        builder.literal(literals[rng.next(static_cast<std::uint32_t>(literals.size()))]);
                        break;
                    case 1:
                        builder.flag(plain[rng.next(static_cast<std::uint32_t>(plain.size()))]);
                        break;
                    case 2:
                        builder.nth(
                            numbered[rng.next(static_cast<std::uint32_t>(numbered.size()))],
                            static_cast<int>(1U + rng.next(98))
                        );
                        break;
                    default:
                        builder.group_ref(groups[rng.next(static_cast<std::uint32_t>(groups.size()))]);
                        break;
                    }
                }
                auto const atoms = builder.take();
                auto const rendered = render_instruction(atoms);
                auto reparsed = pup::test::parse_instruction_text(global_pool().get(rendered));

                auto const ok = reparsed.has_value() && atoms_equal(atoms, *reparsed);
                if (!ok) {
                    ++failures;
                    if (first_failure.empty()) {
                        first_failure = "rendered='" + std::string { global_pool().get(rendered) }
                            + "' atoms=" + describe(atoms)
                            + " reparsed=" + (reparsed ? describe(*reparsed) : std::string { "<error>" });
                    }
                }
            }

            THEN("every instruction survives the round trip")
            {
                INFO("seed: " << seed);
                INFO("first failure: " << first_failure);
                REQUIRE(failures == 0);
            }
        }
    }
}
