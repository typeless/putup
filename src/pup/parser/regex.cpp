// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/parser/regex.hpp"
#include "pup/core/buf.hpp"

#include <cstddef>
#include <optional>
#include <string_view>
#include <utility>

namespace pup::parser {

namespace {

using ByteSet = std::array<std::uint64_t, 4>;

auto contains(ByteSet const& set, char c) -> bool
{
    auto const byte = static_cast<unsigned char>(c);
    return (set[byte / 64] >> (byte % 64)) & 1U;
}

auto add(ByteSet& set, char c) -> void
{
    auto const byte = static_cast<unsigned char>(c);
    set[byte / 64] |= std::uint64_t { 1 } << (byte % 64);
}

auto add_range(ByteSet& set, char low, char high) -> void
{
    for (auto byte = static_cast<unsigned>(static_cast<unsigned char>(low)); byte <= static_cast<unsigned char>(high); ++byte) {
        add(set, static_cast<char>(byte));
    }
}

auto add_all(ByteSet& set, ByteSet const& other) -> void
{
    for (auto i = std::size_t { 0 }; i < set.size(); ++i) {
        set[i] |= other[i];
    }
}

auto complement(ByteSet const& set) -> ByteSet
{
    auto result = ByteSet {};
    for (auto i = std::size_t { 0 }; i < set.size(); ++i) {
        result[i] = ~set[i];
    }
    return result;
}

auto is_alphanumeric(char c) -> bool
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

auto is_quantifier(char c) -> bool
{
    return c == '*' || c == '+' || c == '?';
}

auto shorthand_class(char letter) -> std::optional<ByteSet>
{
    auto set = ByteSet {};
    switch (letter) {
    case 'd':
    case 'D':
        add_range(set, '0', '9');
        break;
    case 'w':
    case 'W':
        add_range(set, 'a', 'z');
        add_range(set, 'A', 'Z');
        add_range(set, '0', '9');
        add(set, '_');
        break;
    case 's':
    case 'S':
        for (auto c : std::string_view { " \t\n\r\f\v" }) {
            add(set, c);
        }
        break;
    default:
        return std::nullopt;
    }
    return letter >= 'A' && letter <= 'Z' ? complement(set) : set;
}

} // namespace

auto Regex::compile(std::string_view pattern) -> Result<Regex>
{
    using Op = Instruction::Op;
    using Fragment = Vec<Instruction>;

    struct ClassItem {
        std::optional<char> single;
        ByteSet set;
        std::size_t offset;
    };

    struct Compiler {
        std::string_view pattern;
        std::size_t pos = 0;
        Vec<CharSet> classes = {};
        std::optional<std::size_t> failure_offset = {};
        std::string_view failure_reason = {};

        auto fail(std::size_t offset, std::string_view reason) -> Fragment
        {
            if (!failure_offset) {
                failure_offset = offset;
                failure_reason = reason;
            }
            return {};
        }

        static auto append(Fragment& into, Fragment const& tail) -> void
        {
            for (auto const& instruction : tail) {
                into.push_back(instruction);
            }
        }

        static auto size_of(Fragment const& fragment) -> std::int32_t { return static_cast<std::int32_t>(fragment.size()); }

        auto emit_class(ByteSet const& set) -> Fragment
        {
            classes.push_back(set);
            auto fragment = Fragment {};
            fragment.push_back(Instruction { .op = Op::Class, .x = static_cast<std::int32_t>(classes.size() - 1) });
            return fragment;
        }

        auto alternation() -> Fragment
        {
            auto left = sequence();
            while (!failure_offset && pos < pattern.size() && pattern[pos] == '|') {
                ++pos;
                auto right = sequence();
                auto joined = Fragment {};
                joined.push_back(Instruction { .op = Op::Split, .x = 1, .y = size_of(left) + 2 });
                append(joined, left);
                joined.push_back(Instruction { .op = Op::Jump, .x = size_of(right) + 1 });
                append(joined, right);
                left = std::move(joined);
            }
            return left;
        }

        auto sequence() -> Fragment
        {
            auto fragment = Fragment {};
            while (!failure_offset && pos < pattern.size() && pattern[pos] != '|' && pattern[pos] != ')') {
                append(fragment, quantified_atom());
            }
            return fragment;
        }

        auto quantified_atom() -> Fragment
        {
            auto const c = pattern[pos];
            if (is_quantifier(c)) {
                return fail(pos, "quantifier does not follow a repeatable item");
            }
            if (c == '^' || c == '$') {
                ++pos;
                auto fragment = Fragment {};
                fragment.push_back(Instruction { .op = c == '^' ? Op::LineStart : Op::LineEnd });
                return fragment;
            }
            auto item = atom();
            if (failure_offset || pos >= pattern.size()) {
                return item;
            }
            auto const quantifier = pattern[pos];
            if (quantifier == '{') {
                return fail(pos, "unsupported syntax '{'");
            }
            if (!is_quantifier(quantifier)) {
                return item;
            }
            ++pos;
            if (pos < pattern.size() && (is_quantifier(pattern[pos]) || pattern[pos] == '{')) {
                return fail(pos, "a quantifier cannot follow a quantifier (lazy and possessive quantifiers are unsupported)");
            }
            auto const length = size_of(item);
            auto fragment = Fragment {};
            switch (quantifier) {
            case '*':
                fragment.push_back(Instruction { .op = Op::Split, .x = 1, .y = length + 2 });
                append(fragment, item);
                fragment.push_back(Instruction { .op = Op::Jump, .x = -(length + 1) });
                break;
            case '+':
                append(fragment, item);
                fragment.push_back(Instruction { .op = Op::Split, .x = -length, .y = 1 });
                break;
            default:
                fragment.push_back(Instruction { .op = Op::Split, .x = 1, .y = length + 1 });
                append(fragment, item);
                break;
            }
            return fragment;
        }

        auto atom() -> Fragment
        {
            auto const c = pattern[pos];
            switch (c) {
            case '{':
                return fail(pos, "unsupported syntax '{'");
            case '(': {
                if (pos + 1 < pattern.size() && pattern[pos + 1] == '?') {
                    return fail(pos + 1, "unsupported syntax '(?'");
                }
                ++pos;
                auto inner = alternation();
                if (failure_offset) {
                    return inner;
                }
                if (pos >= pattern.size()) {
                    return fail(pos, "missing )");
                }
                ++pos;
                return inner;
            }
            case '[':
                return bracket_class();
            case '.': {
                ++pos;
                auto fragment = Fragment {};
                fragment.push_back(Instruction { .op = Op::Any });
                return fragment;
            }
            case '\\': {
                if (pos + 1 >= pattern.size()) {
                    return fail(pos + 1, "\\ at end of pattern");
                }
                auto const escaped = pattern[pos + 1];
                if (auto set = shorthand_class(escaped)) {
                    pos += 2;
                    return emit_class(*set);
                }
                if (is_alphanumeric(escaped)) {
                    return fail(pos + 1, "unsupported escape sequence");
                }
                pos += 2;
                auto fragment = Fragment {};
                fragment.push_back(Instruction { .op = Op::Char, .c = escaped });
                return fragment;
            }
            default: {
                ++pos;
                auto fragment = Fragment {};
                fragment.push_back(Instruction { .op = Op::Char, .c = c });
                return fragment;
            }
            }
        }

        auto class_item() -> std::optional<ClassItem>
        {
            auto const offset = pos;
            if (pattern[pos] != '\\') {
                ++pos;
                return ClassItem { .single = pattern[offset], .set = {}, .offset = offset };
            }
            if (pos + 1 >= pattern.size()) {
                fail(pos + 1, "missing ] for character class");
                return std::nullopt;
            }
            auto const escaped = pattern[pos + 1];
            if (auto set = shorthand_class(escaped)) {
                pos += 2;
                return ClassItem { .single = std::nullopt, .set = *set, .offset = offset };
            }
            if (is_alphanumeric(escaped)) {
                fail(pos + 1, "unsupported escape sequence");
                return std::nullopt;
            }
            pos += 2;
            return ClassItem { .single = escaped, .set = {}, .offset = offset };
        }

        auto bracket_class() -> Fragment
        {
            ++pos;
            auto negated = pos < pattern.size() && pattern[pos] == '^';
            if (negated) {
                ++pos;
            }
            auto set = ByteSet {};
            auto first = true;
            for (;;) {
                if (pos >= pattern.size()) {
                    return fail(pos, "missing ] for character class");
                }
                if (pattern[pos] == ']' && !first) {
                    ++pos;
                    break;
                }
                if (pattern[pos] == '[' && pos + 1 < pattern.size() && (pattern[pos + 1] == ':' || pattern[pos + 1] == '.' || pattern[pos + 1] == '=')) {
                    return fail(pos, "POSIX character classes are unsupported");
                }
                first = false;
                auto low = class_item();
                if (!low) {
                    return {};
                }
                auto const ranged = pos + 1 < pattern.size() && pattern[pos] == '-' && pattern[pos + 1] != ']';
                if (!ranged) {
                    if (low->single) {
                        add(set, *low->single);
                    } else {
                        add_all(set, low->set);
                    }
                    continue;
                }
                if (!low->single) {
                    return fail(pos, "invalid range in character class");
                }
                ++pos;
                auto high = class_item();
                if (!high) {
                    return {};
                }
                if (!high->single) {
                    return fail(high->offset, "invalid range in character class");
                }
                if (static_cast<unsigned char>(*high->single) < static_cast<unsigned char>(*low->single)) {
                    return fail(high->offset, "range out of order in character class");
                }
                add_range(set, *low->single, *high->single);
            }
            return emit_class(negated ? complement(set) : set);
        }
    };

    auto compiler = Compiler { .pattern = pattern };
    auto program = compiler.alternation();
    if (!compiler.failure_offset && compiler.pos < pattern.size()) {
        compiler.fail(compiler.pos, "unmatched )");
    }
    if (compiler.failure_offset) {
        auto message = Buf {};
        message.fmt("Unable to compile regular expression '{}' at offset {}: {}", pattern, *compiler.failure_offset, compiler.failure_reason);
        return make_error<Regex>(ErrorCode::ParseError, message.view());
    }
    program.push_back(Instruction { .op = Op::Match });

    auto regex = Regex {};
    regex.program_ = std::move(program);
    regex.classes_ = std::move(compiler.classes);
    return regex;
}

auto Regex::search(std::string_view subject) const -> bool
{
    using Op = Instruction::Op;

    struct Thread {
        std::int32_t pc;
        std::size_t pos;
    };

    auto const positions = subject.size() + 1;
    auto visited = Vec<std::uint64_t> {};
    visited.resize((program_.size() * positions + 63) / 64);
    auto pending = Vec<Thread> {};

    for (auto start = std::size_t { 0 }; start < positions; ++start) {
        pending.push_back(Thread { .pc = 0, .pos = start });
        while (!pending.empty()) {
            auto thread = pending.back();
            pending.pop_back();
            for (;;) {
                auto const state = static_cast<std::size_t>(thread.pc) * positions + thread.pos;
                if ((visited[state / 64] >> (state % 64)) & 1U) {
                    break;
                }
                visited[state / 64] |= std::uint64_t { 1 } << (state % 64);

                auto const& instruction = program_[static_cast<std::size_t>(thread.pc)];
                auto const at_end = thread.pos == subject.size();
                auto advances = false;
                switch (instruction.op) {
                case Op::Char:
                    advances = !at_end && subject[thread.pos] == instruction.c;
                    break;
                case Op::Any:
                    advances = !at_end && subject[thread.pos] != '\n';
                    break;
                case Op::Class:
                    advances = !at_end && contains(classes_[static_cast<std::size_t>(instruction.x)], subject[thread.pos]);
                    break;
                case Op::Split:
                    pending.push_back(Thread { .pc = thread.pc + instruction.y, .pos = thread.pos });
                    thread.pc += instruction.x;
                    continue;
                case Op::Jump:
                    thread.pc += instruction.x;
                    continue;
                case Op::LineStart:
                    if (thread.pos != 0) {
                        break;
                    }
                    ++thread.pc;
                    continue;
                case Op::LineEnd:
                    if (!at_end && !(thread.pos + 1 == subject.size() && subject[thread.pos] == '\n')) {
                        break;
                    }
                    ++thread.pc;
                    continue;
                case Op::Match:
                    return true;
                }
                if (!advances) {
                    break;
                }
                ++thread.pc;
                ++thread.pos;
            }
        }
    }
    return false;
}

} // namespace pup::parser
