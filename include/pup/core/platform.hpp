// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include "pup/core/string_id.hpp"

#include <cstddef>
#include <string_view>

namespace pup {

/// Platform identifier string (matches tup's TUP_PLATFORM)
#if defined(__linux__)
inline constexpr auto PLATFORM = std::string_view { "linux" };
#elif defined(__APPLE__)
inline constexpr auto PLATFORM = std::string_view { "macosx" };
#elif defined(_WIN32)
inline constexpr auto PLATFORM = std::string_view { "win32" };
#elif defined(__FreeBSD__)
inline constexpr auto PLATFORM = std::string_view { "freebsd" };
#elif defined(__NetBSD__)
inline constexpr auto PLATFORM = std::string_view { "netbsd" };
#elif defined(__sun)
inline constexpr auto PLATFORM = std::string_view { "solaris" };
#else
inline constexpr auto PLATFORM = std::string_view { "unknown" };
#endif

/// Architecture identifier
#if defined(__x86_64__) || defined(_M_X64)
inline constexpr auto ARCH = std::string_view { "x86_64" };
#elif defined(__i386__) || defined(_M_IX86)
inline constexpr auto ARCH = std::string_view { "i386" };
#elif defined(__aarch64__) || defined(_M_ARM64)
inline constexpr auto ARCH = std::string_view { "aarch64" };
#elif defined(__arm__) || defined(_M_ARM)
inline constexpr auto ARCH = std::string_view { "arm" };
#elif defined(__riscv)
inline constexpr auto ARCH = std::string_view { "riscv" };
#else
inline constexpr auto ARCH = std::string_view { "unknown" };
#endif

/// Get platform string at runtime (checks TUP_PLATFORM env var first)
[[nodiscard]]
auto get_platform() -> StringId;

/// Number of available CPU cores (for -j auto-detection)
[[nodiscard]]
auto cpu_count() -> std::size_t;

} // namespace pup
