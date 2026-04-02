// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include <cstddef>
#include <cstdlib>
#include <new>
#include <type_traits>
#include <utility>

namespace pup {

/// Paged vector — append-only growable array with pointer stability.
/// Elements are stored in fixed-size pages. push_back never invalidates
/// pointers to existing elements (new pages are allocated separately).
/// Replaces std::deque for append-only use cases.
template<typename T, std::size_t PageSize = 256>
class PagedVec final {
public:
    PagedVec() = default;

    ~PagedVec()
    {
        for (std::size_t i = 0; i < size_; ++i) {
            at(i).~T();
        }
        for (std::size_t p = 0; p < page_count_; ++p) {
            std::free(pages_[p]);
        }
        std::free(pages_);
    }

    PagedVec(PagedVec const&) = delete;
    auto operator=(PagedVec const&) -> PagedVec& = delete;

    PagedVec(PagedVec&& other) noexcept
        : pages_(other.pages_)
        , size_(other.size_)
        , page_count_(other.page_count_)
        , page_capacity_(other.page_capacity_)
    {
        other.pages_ = nullptr;
        other.size_ = 0;
        other.page_count_ = 0;
        other.page_capacity_ = 0;
    }

    auto operator=(PagedVec&& other) noexcept -> PagedVec&
    {
        if (this != &other) {
            for (std::size_t i = 0; i < size_; ++i) {
                at(i).~T();
            }
            for (std::size_t p = 0; p < page_count_; ++p) {
                std::free(pages_[p]);
            }
            std::free(pages_);

            pages_ = other.pages_;
            size_ = other.size_;
            page_count_ = other.page_count_;
            page_capacity_ = other.page_capacity_;
            other.pages_ = nullptr;
            other.size_ = 0;
            other.page_count_ = 0;
            other.page_capacity_ = 0;
        }
        return *this;
    }

    auto push_back(T const& value) -> void
    {
        ensure_page();
        new (&at(size_)) T(value);
        ++size_;
    }

    auto push_back(T&& value) -> void
    {
        ensure_page();
        new (&at(size_)) T(std::move(value));
        ++size_;
    }

    template<typename... Args>
    auto emplace_back(Args&&... args) -> T&
    {
        ensure_page();
        auto* p = new (&at(size_)) T(std::forward<Args>(args)...);
        ++size_;
        return *p;
    }

    [[nodiscard]]
    auto operator[](std::size_t i) -> T&
    {
        return at(i);
    }

    [[nodiscard]]
    auto operator[](std::size_t i) const -> T const&
    {
        return at(i);
    }

    [[nodiscard]]
    auto back() -> T&
    {
        return at(size_ - 1);
    }

    [[nodiscard]]
    auto back() const -> T const&
    {
        return at(size_ - 1);
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

    auto resize(std::size_t n) -> void
    {
        while (size_ < n) {
            ensure_page();
            new (&at(size_)) T();
            ++size_;
        }
    }

    auto clear() -> void
    {
        for (std::size_t i = 0; i < size_; ++i) {
            at(i).~T();
        }
        size_ = 0;
    }

    struct Iterator {
        PagedVec* pv;
        std::size_t idx;

        auto operator*() -> T& { return pv->at(idx); }
        auto operator++() -> Iterator&
        {
            ++idx;
            return *this;
        }
        auto operator!=(Iterator const& o) const -> bool { return idx != o.idx; }
    };

    struct ConstIterator {
        PagedVec const* pv;
        std::size_t idx;

        auto operator*() const -> T const& { return pv->at(idx); }
        auto operator++() -> ConstIterator&
        {
            ++idx;
            return *this;
        }
        auto operator!=(ConstIterator const& o) const -> bool { return idx != o.idx; }
    };

    auto begin() -> Iterator { return { this, 0 }; }
    auto end() -> Iterator { return { this, size_ }; }
    auto begin() const -> ConstIterator { return { this, 0 }; }
    auto end() const -> ConstIterator { return { this, size_ }; }

private:
    T** pages_ = nullptr;
    std::size_t size_ = 0;
    std::size_t page_count_ = 0;
    std::size_t page_capacity_ = 0;

    auto at(std::size_t i) -> T&
    {
        return pages_[i / PageSize][i % PageSize];
    }

    auto at(std::size_t i) const -> T const&
    {
        return pages_[i / PageSize][i % PageSize];
    }

    auto ensure_page() -> void
    {
        auto needed_page = size_ / PageSize;
        if (needed_page < page_count_) {
            return;
        }
        // Need a new page
        if (page_count_ >= page_capacity_) {
            auto new_cap = page_capacity_ + page_capacity_ / 2 + 4;
            auto* new_pages = static_cast<T**>(std::realloc(pages_, new_cap * sizeof(T*)));
            if (!new_pages) {
                std::abort();
            }
            pages_ = new_pages;
            page_capacity_ = new_cap;
        }
        auto* page = static_cast<T*>(std::malloc(PageSize * sizeof(T)));
        if (!page) {
            std::abort();
        }
        pages_[page_count_++] = page;
    }
};

} // namespace pup
