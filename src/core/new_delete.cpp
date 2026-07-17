// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

// Total replacement of the global allocation functions on the MSVC static
// CRT (linked instead of throw_stubs.cpp, which serves the -nostdlib++
// POSIX builds). Totality is load-bearing: any variant left undefined is
// silently pulled from libcmt, and CRT operator delete on a pointer from
// our operator new corrupts the CRT heap. The pup_ov_* counters exist so
// tests can prove each variant class resolves to this TU.

// Guarded rather than left to the build system alone: `putup show compdb`
// also lists rules from untaken conditional branches, so Linux tooling
// (clang-tidy) would otherwise try to compile this MSVC-only TU.
#ifdef _WIN32

#    include "pup/core/bump_alloc.hpp"

#    include <cstddef>
#    include <new>

extern "C" {
unsigned pup_ov_new = 0;
unsigned pup_ov_new_nothrow = 0;
unsigned pup_ov_new_aligned = 0;
unsigned pup_ov_delete = 0;
unsigned pup_ov_delete_aligned = 0;
}

auto operator new(std::size_t n) -> void*
{
    ++pup_ov_new;
    return pup::bump_alloc(n, __STDCPP_DEFAULT_NEW_ALIGNMENT__);
}

auto operator new[](std::size_t n) -> void*
{
    ++pup_ov_new;
    return pup::bump_alloc(n, __STDCPP_DEFAULT_NEW_ALIGNMENT__);
}

auto operator new(std::size_t n, std::nothrow_t const&) noexcept -> void*
{
    ++pup_ov_new_nothrow;
    return pup::bump_alloc(n, __STDCPP_DEFAULT_NEW_ALIGNMENT__);
}

auto operator new[](std::size_t n, std::nothrow_t const&) noexcept -> void*
{
    ++pup_ov_new_nothrow;
    return pup::bump_alloc(n, __STDCPP_DEFAULT_NEW_ALIGNMENT__);
}

auto operator new(std::size_t n, std::align_val_t align) -> void*
{
    ++pup_ov_new_aligned;
    return pup::bump_alloc(n, static_cast<std::size_t>(align));
}

auto operator new[](std::size_t n, std::align_val_t align) -> void*
{
    ++pup_ov_new_aligned;
    return pup::bump_alloc(n, static_cast<std::size_t>(align));
}

auto operator new(std::size_t n, std::align_val_t align, std::nothrow_t const&) noexcept -> void*
{
    ++pup_ov_new_aligned;
    return pup::bump_alloc(n, static_cast<std::size_t>(align));
}

auto operator new[](std::size_t n, std::align_val_t align, std::nothrow_t const&) noexcept -> void*
{
    ++pup_ov_new_aligned;
    return pup::bump_alloc(n, static_cast<std::size_t>(align));
}

auto operator delete(void*) noexcept -> void
{
    ++pup_ov_delete;
}

auto operator delete[](void*) noexcept -> void
{
    ++pup_ov_delete;
}

auto operator delete(void*, std::size_t) noexcept -> void
{
    ++pup_ov_delete;
}

auto operator delete[](void*, std::size_t) noexcept -> void
{
    ++pup_ov_delete;
}

auto operator delete(void*, std::nothrow_t const&) noexcept -> void
{
    ++pup_ov_delete;
}

auto operator delete[](void*, std::nothrow_t const&) noexcept -> void
{
    ++pup_ov_delete;
}

auto operator delete(void*, std::align_val_t) noexcept -> void
{
    ++pup_ov_delete_aligned;
}

auto operator delete[](void*, std::align_val_t) noexcept -> void
{
    ++pup_ov_delete_aligned;
}

auto operator delete(void*, std::size_t, std::align_val_t) noexcept -> void
{
    ++pup_ov_delete_aligned;
}

auto operator delete[](void*, std::size_t, std::align_val_t) noexcept -> void
{
    ++pup_ov_delete_aligned;
}

auto operator delete(void*, std::align_val_t, std::nothrow_t const&) noexcept -> void
{
    ++pup_ov_delete_aligned;
}

auto operator delete[](void*, std::align_val_t, std::nothrow_t const&) noexcept -> void
{
    ++pup_ov_delete_aligned;
}

#endif // _WIN32
