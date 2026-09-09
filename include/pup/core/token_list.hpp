// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include "pup/core/vec.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <span>
#include <utility>

namespace pup {

/// A rule's operands, grouped by the written token each came from. Upstream numbers the tokens a
/// rule spells rather than the operands they expand to, so `%2f` names every operand of the second
/// written token and a token that expanded to nothing still holds its number.
///
/// The grouping is a prefix sum, built only by the constructors here, so no producer can hand a
/// reader a boundary that disagrees with the operands it groups. Reading the operands themselves
/// needs no knowledge of it: the sequence interface below is the whole list, in written order.
template<typename T>
class TokenList final {
public:
    TokenList() = default;

    /// One operand per token, the shape every producer but a rule's own input and output lists has.
    TokenList(Vec<T> ids)
        : m_ids { std::move(ids) }
    {
        m_starts.clear();
        m_starts.reserve(m_ids.size() + 1);
        for (auto i = std::size_t { 0 }; i <= m_ids.size(); ++i) {
            m_starts.push_back(static_cast<std::uint32_t>(i));
        }
    }

    TokenList(std::initializer_list<T> init)
        : TokenList { Vec<T> { init } }
    {
    }

    /// `token_of[i]` is the 1-based number of the token operand `i` came from, non-decreasing.
    /// `token_count` is how many tokens were written, which the last ones owning no operand and
    /// the trailing empty ones alike are counted by.
    [[nodiscard]]
    static auto grouped(Vec<T> ids, Vec<std::uint32_t> const& token_of, std::uint32_t token_count)
        -> TokenList
    {
        auto list = TokenList {};
        list.m_ids = std::move(ids);
        list.m_starts.clear();
        list.m_starts.reserve(std::size_t { token_count } + 1);
        auto next = std::size_t { 0 };
        for (auto token = std::uint32_t { 0 }; token <= token_count; ++token) {
            while (next < token_of.size() && token_of[next] <= token) {
                ++next;
            }
            list.m_starts.push_back(static_cast<std::uint32_t>(next));
        }
        assert(list.m_starts.front() == 0 && list.m_starts.back() == list.m_ids.size());
        return list;
    }

    /// The recorded boundary, read back. Rejects a prefix sum that does not run from zero to the
    /// operand count without decreasing, so a damaged record cannot reach a reader as a grouping.
    [[nodiscard]]
    static auto from_starts(Vec<T> ids, Vec<std::uint32_t> starts) -> std::optional<TokenList>
    {
        if (starts.empty() || starts[0] != 0 || starts[starts.size() - 1] != ids.size()) {
            return std::nullopt;
        }
        for (auto i = std::size_t { 1 }; i < starts.size(); ++i) {
            if (starts[i] < starts[i - 1]) {
                return std::nullopt;
            }
        }
        auto list = TokenList {};
        list.m_ids = std::move(ids);
        list.m_starts = std::move(starts);
        return list;
    }

    /// Drops one operand, leaving every survivor under the number it was written with -- so a token
    /// that loses its last operand renders empty rather than taking the next token's operands.
    auto drop(std::size_t index) -> void
    {
        if (index >= m_ids.size()) {
            return;
        }
        m_ids.erase(m_ids.begin() + static_cast<std::ptrdiff_t>(index));
        for (auto& start : m_starts) {
            if (start > index) {
                --start;
            }
        }
    }

    /// Puts a different operand in one slot, leaving the grouping alone.
    auto replace(std::size_t index, T value) -> void
    {
        if (index < m_ids.size()) {
            m_ids[index] = std::move(value);
        }
    }

    /// The operands of the `number`-th written token, empty when that token owns none or was never
    /// written. 1-based, as a rule spells it.
    [[nodiscard]]
    auto token(std::uint32_t number) const -> std::span<T const>
    {
        if (number == 0 || std::size_t { number } >= m_starts.size()) {
            return {};
        }
        auto const first = m_starts[number - 1];
        auto const last = m_starts[number];
        return std::span<T const> { m_ids.begin() + first, m_ids.begin() + last };
    }

    [[nodiscard]]
    auto token_count() const -> std::uint32_t
    {
        return m_starts.empty() ? 0 : static_cast<std::uint32_t>(m_starts.size() - 1);
    }

    [[nodiscard]]
    auto starts() const -> Vec<std::uint32_t> const&
    {
        return m_starts;
    }

    [[nodiscard]]
    auto ids() const -> Vec<T> const&
    {
        return m_ids;
    }

    [[nodiscard]]
    auto size() const -> std::size_t
    {
        return m_ids.size();
    }

    [[nodiscard]]
    auto empty() const -> bool
    {
        return m_ids.empty();
    }

    [[nodiscard]]
    auto operator[](std::size_t i) const -> T const&
    {
        return m_ids[i];
    }

    [[nodiscard]]
    auto begin() const -> T const*
    {
        return m_ids.begin();
    }

    [[nodiscard]]
    auto end() const -> T const*
    {
        return m_ids.end();
    }

    [[nodiscard]]
    auto operator==(TokenList const& other) const -> bool
    {
        return m_ids == other.m_ids && m_starts == other.m_starts;
    }

    [[nodiscard]]
    auto operator==(Vec<T> const& other) const -> bool
    {
        return m_ids == other;
    }

private:
    Vec<T> m_ids {};
    Vec<std::uint32_t> m_starts { 0 };
};

} // namespace pup
