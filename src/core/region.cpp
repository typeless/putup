// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/core/region.hpp"
#include "pup/platform/vm.hpp"

#include <cstdio>
#include <cstdlib>
#include <utility>

namespace pup {

namespace vm = platform::vm;

namespace {

auto round_up_to_page(std::size_t bytes) -> std::size_t
{
    auto const page = vm::page_size();
    return (bytes + page - 1) & ~(page - 1);
}

} // namespace

Region::Region(std::size_t reserve_bytes)
    : reserve_bytes_(reserve_bytes)
{
}

Region::~Region()
{
    if (base_) {
        vm::release(base_, reserved_);
    }
}

Region::Region(Region&& other) noexcept
    : base_(std::exchange(other.base_, nullptr))
    , reserved_(std::exchange(other.reserved_, 0))
    , committed_(std::exchange(other.committed_, 0))
    , reserve_bytes_(other.reserve_bytes_)
{
}

auto Region::operator=(Region&& other) noexcept -> Region&
{
    if (this != &other) {
        if (base_) {
            vm::release(base_, reserved_);
        }
        base_ = std::exchange(other.base_, nullptr);
        reserved_ = std::exchange(other.reserved_, 0);
        committed_ = std::exchange(other.committed_, 0);
        reserve_bytes_ = other.reserve_bytes_;
    }
    return *this;
}

auto Region::ensure(std::size_t bytes) -> void
{
    if (bytes <= committed_) {
        return;
    }
    if (!base_) {
        reserved_ = round_up_to_page(reserve_bytes_);
        base_ = vm::reserve(reserved_);
        if (!base_) {
            std::fprintf(stderr, "fatal: region reserve of %zu bytes failed\n", reserved_);
            std::abort();
        }
    }
    auto const new_committed = round_up_to_page(bytes);
    if (new_committed > reserved_) {
        std::fprintf(stderr, "fatal: region reservation exhausted (%zu > %zu bytes)\n", new_committed, reserved_);
        std::abort();
    }
    vm::commit(static_cast<char*>(base_) + committed_, new_committed - committed_);
    committed_ = new_committed;
}

auto Region::shrink(std::size_t keep_bytes) -> void
{
    if (keep_bytes == 0) {
        if (base_) {
            vm::release(base_, reserved_);
            base_ = nullptr;
            reserved_ = 0;
            committed_ = 0;
        }
        return;
    }
    auto const keep = round_up_to_page(keep_bytes);
    if (keep >= committed_) {
        return;
    }
    vm::decommit(static_cast<char*>(base_) + keep, committed_ - keep);
    committed_ = keep;
}

auto Region::data() const -> void*
{
    return base_;
}

auto Region::committed() const -> std::size_t
{
    return committed_;
}

auto Region::reserved() const -> std::size_t
{
    return reserved_;
}

} // namespace pup
