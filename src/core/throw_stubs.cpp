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
// __cxa_atexit comes from libc and does not need a stub.

#include <cstdio>
#include <cstdlib>

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

// C++ new/delete operators
void* operator new(std::size_t n) // NOLINT
{
    auto* p = std::malloc(n);
    if (!p) {
        std::abort();
    }
    return p;
}

void* operator new[](std::size_t n) // NOLINT
{
    auto* p = std::malloc(n);
    if (!p) {
        std::abort();
    }
    return p;
}
void operator delete(void* p) noexcept { std::free(p); }                // NOLINT
void operator delete[](void* p) noexcept { std::free(p); }              // NOLINT
void operator delete(void* p, std::size_t) noexcept { std::free(p); }   // NOLINT
void operator delete[](void* p, std::size_t) noexcept { std::free(p); } // NOLINT

namespace std { // NOLINT(cert-dcl58-cpp)

[[noreturn]]
void __throw_bad_alloc()
{
    fputs("fatal: bad_alloc\n", stderr);
    abort();
}

[[noreturn]]
void __throw_bad_array_new_length()
{
    fputs("fatal: bad_array_new_length\n", stderr);
    abort();
}

[[noreturn]]
void __throw_length_error(char const* msg)
{
    fprintf(stderr, "fatal: length_error: %s\n", msg);
    abort();
}

[[noreturn]]
void __throw_out_of_range_fmt(char const* /* fmt */, ...)
{
    fputs("fatal: out_of_range\n", stderr);
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
    fputs("fatal: libc++ abort\n", stderr);
    abort();
}
} // namespace __1
} // namespace std
#endif
