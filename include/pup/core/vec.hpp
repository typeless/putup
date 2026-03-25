// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <new>
#include <type_traits>
#include <utility>

namespace pup {

template<typename T>
class Vec {
public:
    Vec() = default;

    ~Vec()
    {
        destroy_range(data_, size_);
        std::free(data_);
    }

    Vec(Vec const& other)
        : data_(alloc(other.size_))
        , size_(other.size_)
        , capacity_(other.size_)
    {
        copy_range(data_, other.data_, size_);
    }

    auto operator=(Vec const& other) -> Vec&
    {
        if (this != &other) {
            destroy_range(data_, size_);
            if (capacity_ < other.size_) {
                std::free(data_);
                data_ = alloc(other.size_);
                capacity_ = other.size_;
            }
            size_ = other.size_;
            copy_range(data_, other.data_, size_);
        }
        return *this;
    }

    Vec(Vec&& other) noexcept
        : data_(other.data_)
        , size_(other.size_)
        , capacity_(other.capacity_)
    {
        other.data_ = nullptr;
        other.size_ = 0;
        other.capacity_ = 0;
    }

    auto operator=(Vec&& other) noexcept -> Vec&
    {
        if (this != &other) {
            destroy_range(data_, size_);
            std::free(data_);
            data_ = other.data_;
            size_ = other.size_;
            capacity_ = other.capacity_;
            other.data_ = nullptr;
            other.size_ = 0;
            other.capacity_ = 0;
        }
        return *this;
    }

    auto push_back(T const& value) -> void
    {
        ensure_capacity(size_ + 1);
        construct_at(data_ + size_, value);
        ++size_;
    }

    auto push_back(T&& value) -> void
    {
        ensure_capacity(size_ + 1);
        construct_at(data_ + size_, std::move(value));
        ++size_;
    }

    template<typename... Args>
    auto emplace_back(Args&&... args) -> T&
    {
        ensure_capacity(size_ + 1);
        auto* p = new (data_ + size_) T(std::forward<Args>(args)...);
        ++size_;
        return *p;
    }

    auto pop_back() -> void
    {
        --size_;
        destroy_at(data_ + size_);
    }

    auto insert(T const* pos, T value) -> T*
    {
        auto idx = static_cast<std::size_t>(pos - data_);
        ensure_capacity(size_ + 1);
        // After ensure_capacity, data_ may have moved — recompute pos
        shift_right(data_ + idx, size_ - idx);
        construct_at(data_ + idx, std::move(value));
        ++size_;
        return data_ + idx;
    }

    auto erase(T const* pos) -> T*
    {
        auto idx = static_cast<std::size_t>(pos - data_);
        auto tail = size_ - idx - 1;
        if constexpr (std::is_trivially_copyable_v<T>) {
            std::memmove(data_ + idx, data_ + idx + 1, tail * sizeof(T));
        } else {
            for (std::size_t i = 0; i < tail; ++i) {
                data_[idx + i] = std::move(data_[idx + i + 1]);
            }
            data_[size_ - 1].~T();
        }
        --size_;
        return data_ + idx;
    }

    auto reserve(std::size_t n) -> void
    {
        if (n > capacity_) {
            grow_to(n);
        }
    }

    auto resize(std::size_t n) -> void
    {
        if (n > size_) {
            ensure_capacity(n);
            default_construct_range(data_ + size_, n - size_);
        } else {
            destroy_range(data_ + n, size_ - n);
        }
        size_ = n;
    }

    auto clear() -> void
    {
        destroy_range(data_, size_);
        size_ = 0;
    }

    [[nodiscard]]
    auto data() -> T*
    {
        return data_;
    }
    [[nodiscard]]
    auto data() const -> T const*
    {
        return data_;
    }
    [[nodiscard]]
    auto size() const -> std::size_t
    {
        return size_;
    }
    [[nodiscard]]
    auto empty() const -> bool
    {
        return size_ == 0;
    }
    [[nodiscard]]
    auto capacity() const -> std::size_t
    {
        return capacity_;
    }

    [[nodiscard]]
    auto operator[](std::size_t i) -> T&
    {
        return data_[i];
    }
    [[nodiscard]]
    auto operator[](std::size_t i) const -> T const&
    {
        return data_[i];
    }

    [[nodiscard]]
    auto front() -> T&
    {
        return data_[0];
    }
    [[nodiscard]]
    auto front() const -> T const&
    {
        return data_[0];
    }
    [[nodiscard]]
    auto back() -> T&
    {
        return data_[size_ - 1];
    }
    [[nodiscard]]
    auto back() const -> T const&
    {
        return data_[size_ - 1];
    }

    [[nodiscard]]
    auto begin() -> T*
    {
        return data_;
    }
    [[nodiscard]]
    auto end() -> T*
    {
        return data_ + size_;
    }
    [[nodiscard]]
    auto begin() const -> T const*
    {
        return data_;
    }
    [[nodiscard]]
    auto end() const -> T const*
    {
        return data_ + size_;
    }

private:
    T* data_ = nullptr;
    std::size_t size_ = 0;
    std::size_t capacity_ = 0;

    static auto alloc(std::size_t n) -> T*
    {
        if (n == 0) {
            return nullptr;
        }
        auto* p = static_cast<T*>(std::malloc(n * sizeof(T)));
        if (!p) {
            std::abort();
        }
        return p;
    }

    auto ensure_capacity(std::size_t needed) -> void
    {
        if (needed <= capacity_) {
            return;
        }
        auto new_cap = capacity_ + capacity_ / 2 + 4;
        if (new_cap < needed) {
            new_cap = needed;
        }
        grow_to(new_cap);
    }

    auto grow_to(std::size_t new_cap) -> void
    {
        if constexpr (std::is_trivially_copyable_v<T>) {
            auto* p = static_cast<T*>(std::realloc(data_, new_cap * sizeof(T)));
            if (!p) {
                std::abort();
            }
            data_ = p;
        } else {
            auto* new_data = alloc(new_cap);
            for (std::size_t i = 0; i < size_; ++i) {
                new (new_data + i) T(std::move(data_[i]));
                data_[i].~T();
            }
            std::free(data_);
            data_ = new_data;
        }
        capacity_ = new_cap;
    }

    static auto construct_at(T* p, T const& value) -> void
    {
        if constexpr (std::is_trivially_copyable_v<T>) {
            *p = value;
        } else {
            new (p) T(value);
        }
    }

    static auto construct_at(T* p, T&& value) -> void
    {
        if constexpr (std::is_trivially_copyable_v<T>) {
            *p = std::move(value);
        } else {
            new (p) T(std::move(value));
        }
    }

    static auto destroy_at(T* p) -> void
    {
        if constexpr (!std::is_trivially_destructible_v<T>) {
            p->~T();
        }
    }

    static auto destroy_range(T* p, std::size_t n) -> void
    {
        if constexpr (!std::is_trivially_destructible_v<T>) {
            for (std::size_t i = 0; i < n; ++i) {
                p[i].~T();
            }
        }
    }

    static auto copy_range(T* dst, T const* src, std::size_t n) -> void
    {
        if (n == 0) {
            return;
        }
        if constexpr (std::is_trivially_copyable_v<T>) {
            std::memcpy(dst, src, n * sizeof(T));
        } else {
            for (std::size_t i = 0; i < n; ++i) {
                new (dst + i) T(src[i]);
            }
        }
    }

    static auto default_construct_range(T* p, std::size_t n) -> void
    {
        if (n == 0) {
            return;
        }
        if constexpr (std::is_trivially_constructible_v<T>) {
            std::memset(p, 0, n * sizeof(T));
        } else {
            for (std::size_t i = 0; i < n; ++i) {
                new (p + i) T();
            }
        }
    }

    static auto shift_right(T* p, std::size_t n) -> void
    {
        if constexpr (std::is_trivially_copyable_v<T>) {
            std::memmove(p + 1, p, n * sizeof(T));
        } else {
            for (auto i = n; i > 0; --i) {
                new (p + i) T(std::move(p[i - 1]));
                p[i - 1].~T();
            }
        }
    }
};

} // namespace pup
