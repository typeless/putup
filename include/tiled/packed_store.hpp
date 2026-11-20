// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Putup authors

#pragma once

#include "tiled/platform.hpp"
#include "tiled/pool.hpp"
#include "tiled/stack.hpp"
#include "tiled/stack_slot.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

namespace tiled {

template<typename T>
concept PackedPart = std::is_trivially_copyable_v<T> && std::has_unique_object_representations_v<T>;

template<PackedPart T>
[[nodiscard]]
auto load_packed(std::byte const* at) -> T
{
    auto bytes = std::array<std::byte, sizeof(T)> {};
    std::memcpy(bytes.data(), at, sizeof(T));
    return std::bit_cast<T>(bytes);
}

template<PackedPart E>
class PackedSequence final {
public:
    class Iterator final {
    public:
        explicit Iterator(std::byte const* at)
            : at_(at)
        {
        }

        auto operator*() const -> E
        {
            return load_packed<E>(at_);
        }

        auto operator++() -> Iterator&
        {
            at_ += sizeof(E);
            return *this;
        }

        auto operator==(Iterator const&) const -> bool = default;

    private:
        std::byte const* at_;
    };

    PackedSequence(std::byte const* first, std::uint32_t size)
        : first_(first)
        , size_(size)
    {
    }

    [[nodiscard]]
    auto size() const -> std::size_t
    {
        return size_;
    }

    [[nodiscard]]
    auto operator[](std::size_t index) const -> E
    {
        return load_packed<E>(first_ + (index * sizeof(E)));
    }

    [[nodiscard]]
    auto begin() const -> Iterator
    {
        return Iterator { first_ };
    }

    [[nodiscard]]
    auto end() const -> Iterator
    {
        return Iterator { first_ + (std::size_t { size_ } * sizeof(E)) };
    }

private:
    std::byte const* first_;
    std::uint32_t size_;
};

template<PackedPart Head, PackedPart... Elements>
requires(sizeof...(Elements) > 0)
class PackedStore;

class RecordId final {
public:
    [[nodiscard]]
    auto offset() const -> std::uint32_t
    {
        return offset_;
    }

private:
    template<PackedPart Head, PackedPart... Elements>
    requires(sizeof...(Elements) > 0)
    friend class PackedStore;

    explicit RecordId(std::uint32_t offset)
        : offset_(offset)
    {
    }

    std::uint32_t offset_;
};

template<PackedPart Head, PackedPart... Elements>
requires(sizeof...(Elements) > 0)
class PackedStore final {
    using Count = std::uint32_t;

    static constexpr auto SEQUENCES = sizeof...(Elements);
    static constexpr auto MIN_CLAIM = std::uint64_t { 64 };
    static constexpr auto TOO_LARGE = std::string_view { "tiled: a packed store holds at most 2^32 bytes" };
    static constexpr auto INTERRUPTED = std::string_view {
        "tiled: a packed record's build was interrupted: another write reached its stack, the builder was closed, or the tile is exhausted"
    };

    template<std::size_t I>
    using Element = std::tuple_element_t<I, std::tuple<Elements...>>;

public:
    template<std::size_t I>
    class Builder final {
    public:
        Builder(Builder const&) = delete;
        auto operator=(Builder const&) -> Builder& = delete;
        auto operator=(Builder&&) -> Builder& = delete;

        Builder(Builder&& other) noexcept
            : stack_(std::exchange(other.stack_, nullptr))
            , base_(other.base_)
            , record_(other.record_)
            , slot_(other.slot_)
            , top_(std::exchange(other.top_, 0))
            , claimed_(std::exchange(other.claimed_, 0))
        {
        }

        ~Builder()
        {
            if (stack_ != nullptr) {
                stack_->pop_top(base_ + record_, claimed_ - record_);
            }
        }

        auto push(Element<I> const& element) -> void
        {
#ifndef NDEBUG
            if (!intact()) {
                fatal(INTERRUPTED.data(), INTERRUPTED.size());
            }
#endif
            if (claimed_ - top_ < sizeof(Element<I>)) [[unlikely]] {
                claim(sizeof(Element<I>));
            }
            std::memcpy(base_ + top_, &element, sizeof(Element<I>));
            top_ += sizeof(Element<I>);
        }

        [[nodiscard]]
        auto next() && -> Builder<I + 1>
        requires(I + 1 < SEQUENCES)
        {
            close_run();
            if (claimed_ - top_ < sizeof(Count)) {
                claim(sizeof(Count));
            }
            auto following = Builder<I + 1> { *stack_, base_, record_, top_, top_ + sizeof(Count), claimed_ };
            stack_ = nullptr;
            top_ = 0;
            claimed_ = 0;
            return following;
        }

        [[nodiscard]]
        auto close() && -> RecordId
        requires(I + 1 == SEQUENCES)
        {
            close_run();
            if (!stack_->extend_top(base_ + record_, claimed_ - record_, top_ - record_)) {
                fatal(INTERRUPTED.data(), INTERRUPTED.size());
            }
            stack_ = nullptr;
            top_ = 0;
            claimed_ = 0;
            return RecordId { static_cast<std::uint32_t>(record_) };
        }

        [[nodiscard]]
        auto intact() const -> bool
        {
            return stack_ != nullptr && stack_->top() == claimed_;
        }

    private:
        friend class PackedStore;

        template<std::size_t>
        friend class Builder;

        Builder(Stack& stack, std::byte* base, std::uint64_t record, std::uint64_t slot, std::uint64_t top, std::uint64_t claimed)
            : stack_(&stack)
            , base_(base)
            , record_(record)
            , slot_(slot)
            , top_(top)
            , claimed_(claimed)
        {
        }

        auto claim(std::uint64_t bytes) -> void
        {
            if (stack_ == nullptr) {
                fatal(INTERRUPTED.data(), INTERRUPTED.size());
            }
            auto const held = claimed_ - record_;
            auto const needed = top_ + bytes - record_;
            auto const doubled = held * 2 > MIN_CLAIM ? held * 2 : MIN_CLAIM;
            auto const wanted = doubled > needed ? doubled : needed;
            if (!stack_->extend_top(base_ + record_, held, wanted)) {
                fatal(INTERRUPTED.data(), INTERRUPTED.size());
            }
            claimed_ = record_ + wanted;
        }

        auto close_run() -> void
        {
            if (stack_ == nullptr) {
                fatal(INTERRUPTED.data(), INTERRUPTED.size());
            }
            auto const count = static_cast<Count>((top_ - slot_ - sizeof(Count)) / sizeof(Element<I>));
            std::memcpy(base_ + slot_, &count, sizeof(Count));
        }

        Stack* stack_;
        std::byte* base_;
        std::uint64_t record_;
        std::uint64_t slot_;
        std::uint64_t top_;
        std::uint64_t claimed_;
    };

    class View final {
    public:
        [[nodiscard]]
        auto head() const -> Head
        {
            return load_packed<Head>(record_);
        }

        template<std::size_t I>
        requires(I < SEQUENCES)
        [[nodiscard]]
        auto sequence() const -> PackedSequence<Element<I>>
        {
            auto const* at = start_of(record_ + sizeof(Head), std::make_index_sequence<I> {});
            return PackedSequence<Element<I>> { at + sizeof(Count), load_packed<Count>(at) };
        }

    private:
        friend class PackedStore;

        explicit View(std::byte const* record)
            : record_(record)
        {
        }

        template<std::size_t... J>
        static auto start_of(std::byte const* at, std::index_sequence<J...>) -> std::byte const*
        {
            ((at += sizeof(Count) + (std::size_t { load_packed<Count>(at) } * sizeof(Element<J>))), ...);
            return at;
        }

        std::byte const* record_;
    };

    PackedStore(Pool& pool, std::uint8_t bytes_log2)
    {
        if (bytes_log2 > 32) {
            fatal(TOO_LARGE.data(), TOO_LARGE.size());
        }
        base_ = static_cast<std::byte*>(stack_.emplace(pool, bytes_log2).base());
    }

    [[nodiscard]]
    auto open(Head const& head) -> Builder<0>
    {
        auto* const at = static_cast<std::byte*>(stack_->push(sizeof(Head) + sizeof(Count), 1));
        std::memcpy(at, &head, sizeof(Head));
        auto const record = static_cast<std::uint64_t>(at - base_);
        auto const slot = record + sizeof(Head);
        return Builder<0> { *stack_, base_, record, slot, slot + sizeof(Count), slot + sizeof(Count) };
    }

    [[nodiscard]]
    auto view(RecordId id) const -> View
    {
        return View { base_ + id.offset() };
    }

private:
    StackSlot stack_;
    std::byte* base_;
};

} // namespace tiled
