// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

// Provide C++ runtime stubs for -nostdlib++ linking.
//
// With -fno-exceptions, STL headers (optional, variant, unique_ptr,
// from_chars) still emit calls to throw functions. These stubs abort
// instead — the code paths are unreachable in correct code.
//
// The C++ ABI symbols (__cxa_guard_*, operator new/delete) normally
// come from libstdc++/libc++. We provide minimal implementations.

#include "pup/core/bump_alloc.hpp"
#include "pup/platform/sys.hpp"

#include <cstddef>
#include <cstdlib>

namespace {
// Keep this TU free of heavy STL headers: they forward-declare the same
// std::__throw_* functions we define below, and the [[noreturn]] on those
// forward decls would clash with ours.
template<std::size_t N>
auto fatal(char const (&msg)[N]) -> void
{
    pup::platform::sys::write(2, msg, N - 1);
}
} // namespace

// C++ ABI: thread-safe static local initialization guards.
// Since we are single-threaded, a simple flag suffices.
extern "C" {

auto __cxa_guard_acquire(long long* guard) -> int // NOLINT
{
    return (*reinterpret_cast<char*>(guard) == 0) ? 1 : 0;
}

auto __cxa_guard_release(long long* guard) -> void // NOLINT
{
    *reinterpret_cast<char*>(guard) = 1;
}

void __cxa_guard_abort(long long*) { } // NOLINT

} // extern "C"

// C++ new/delete: bump region, never reused (batch lifetime — the region
// dies with the process, so delete has nothing to do).
void* operator new(std::size_t n) // NOLINT
{
    return pup::bump_alloc(n, __STDCPP_DEFAULT_NEW_ALIGNMENT__);
}

void* operator new[](std::size_t n) // NOLINT
{
    return pup::bump_alloc(n, __STDCPP_DEFAULT_NEW_ALIGNMENT__);
}
void operator delete(void*) noexcept { }                // NOLINT
void operator delete[](void*) noexcept { }              // NOLINT
void operator delete(void*, std::size_t) noexcept { }   // NOLINT
void operator delete[](void*, std::size_t) noexcept { } // NOLINT

namespace std { // NOLINT(cert-dcl58-cpp)

[[noreturn]]
void __throw_bad_alloc()
{
    fatal("fatal: bad_alloc\n");
    abort();
}

[[noreturn]]
void __throw_bad_array_new_length()
{
    fatal("fatal: bad_array_new_length\n");
    abort();
}

[[noreturn]]
void __throw_length_error(char const* /* msg */)
{
    fatal("fatal: length_error\n");
    abort();
}

[[noreturn]]
void __throw_out_of_range_fmt(char const* /* fmt */, ...)
{
    fatal("fatal: out_of_range\n");
    abort();
}

} // namespace std

// libc++ (macOS) uses std::__1::__libcpp_verbose_abort for variant/optional
// errors. The __1 is libc++'s inline ABI namespace. We match it directly.
#ifdef __APPLE__
namespace std { // NOLINT(cert-dcl58-cpp)
inline namespace __1 {
[[noreturn]]
void __libcpp_verbose_abort(char const* /* fmt */, ...)
{
    fatal("fatal: libc++ abort\n");
    abort();
}
} // namespace __1
} // namespace std
#endif
