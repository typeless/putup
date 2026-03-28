// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include <cstddef>
#include <cstdlib>
#include <new>
#include <type_traits>
#include <utility>

namespace pup {

template<typename Sig>
class Function;

template<typename R, typename... Args>
class Function<R(Args...)> {
public:
    Function() = default;

    Function(std::nullptr_t) { } // NOLINT(google-explicit-constructor)

    template<typename F>
        requires(!std::is_same_v<std::decay_t<F>, Function>)
    Function(F&& f) // NOLINT(google-explicit-constructor)
    {
        using Fn = std::decay_t<F>;
        static_assert(std::is_invocable_r_v<R, Fn, Args...>);

        if constexpr (sizeof(Fn) <= BUF_SIZE && alignof(Fn) <= alignof(Storage) && std::is_nothrow_move_constructible_v<Fn>) {
            new (buf_.data) Fn(std::forward<F>(f));
            heap_ = false;
        } else {
            buf_.ptr = new (std::nothrow) Fn(std::forward<F>(f));
            if (!buf_.ptr) {
                std::abort();
            }
            heap_ = true;
        }

        invoke_ = [](void* p, Args... args) -> R {
            return (*static_cast<Fn*>(p))(std::forward<Args>(args)...);
        };
        destroy_ = [](void* p) {
            static_cast<Fn*>(p)->~Fn();
        };
        move_ = [](void* dst, void* src) {
            new (dst) Fn(std::move(*static_cast<Fn*>(src)));
            static_cast<Fn*>(src)->~Fn();
        };
    }

    ~Function()
    {
        reset();
    }

    Function(Function const&) = delete;
    auto operator=(Function const&) -> Function& = delete;

    Function(Function&& other) noexcept
    {
        move_from(other);
    }

    auto operator=(Function&& other) noexcept -> Function&
    {
        if (this != &other) {
            reset();
            move_from(other);
        }
        return *this;
    }

    auto operator=(std::nullptr_t) noexcept -> Function&
    {
        reset();
        return *this;
    }

    auto operator()(Args... args) const -> R
    {
        if (!invoke_) {
            std::abort();
        }
        return invoke_(data(), std::forward<Args>(args)...);
    }

    explicit operator bool() const noexcept
    {
        return invoke_ != nullptr;
    }

private:
    static constexpr std::size_t BUF_SIZE = 32;

    union Storage {
        alignas(std::max_align_t) char data[BUF_SIZE];
        void* ptr;
    };

    Storage buf_ {};
    R (*invoke_)(void*, Args...) = nullptr;
    void (*destroy_)(void*) = nullptr;
    void (*move_)(void* dst, void* src) = nullptr;
    bool heap_ = false;

    auto data() const -> void*
    {
        return heap_ ? buf_.ptr : const_cast<char*>(buf_.data);
    }

    auto reset() -> void
    {
        if (invoke_) {
            destroy_(data());
            if (heap_) {
                ::operator delete(buf_.ptr);
            }
            invoke_ = nullptr;
            destroy_ = nullptr;
            move_ = nullptr;
            heap_ = false;
        }
    }

    auto move_from(Function& other) -> void
    {
        if (!other.invoke_) {
            return;
        }

        invoke_ = other.invoke_;
        destroy_ = other.destroy_;
        move_ = other.move_;
        heap_ = other.heap_;

        if (heap_) {
            buf_.ptr = other.buf_.ptr;
        } else {
            move_(buf_.data, other.buf_.data);
        }

        other.invoke_ = nullptr;
        other.destroy_ = nullptr;
        other.move_ = nullptr;
        other.heap_ = false;
    }
};

} // namespace pup
