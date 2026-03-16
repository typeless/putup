// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace pup {

/// A filesystem path is a UTF-8 encoded string using '/' as separator.
/// Windows native conversion happens at the platform boundary (file_io-win32.cpp).
using Path = std::string;

/// Path string operations. All functions operate on forward-slash-separated
/// UTF-8 paths without touching the filesystem.
namespace path {

/// Join two path segments with '/'.
/// join("src", "foo.c") → "src/foo.c"
/// join("src/", "foo.c") → "src/foo.c"
/// join("", "foo.c") → "foo.c"
/// join("src", "/usr/include") → "/usr/include" (absolute rhs replaces)
[[nodiscard]]
auto join(std::string_view a, std::string_view b) -> std::string;

/// Get the parent directory.
/// parent("src/lib/foo.c") → "src/lib"
/// parent("foo.c") → ""
/// parent("") → ""
/// parent("/") → "/"
[[nodiscard]]
auto parent(std::string_view p) -> std::string_view;

/// Get the filename component (after last '/').
/// filename("src/foo.c") → "foo.c"
/// filename("foo.c") → "foo.c"
/// filename("src/") → ""
[[nodiscard]]
auto filename(std::string_view p) -> std::string_view;

/// Get the stem (filename without extension).
/// stem("src/foo.tar.gz") → "foo.tar"
/// stem("Makefile") → "Makefile"
[[nodiscard]]
auto stem(std::string_view p) -> std::string_view;

/// Get the file extension (including dot).
/// extension("foo.c") → ".c"
/// extension("foo.tar.gz") → ".gz"
/// extension("Makefile") → ""
[[nodiscard]]
auto extension(std::string_view p) -> std::string_view;

/// Check if a path is absolute.
/// POSIX: starts with '/'
/// Windows: starts with drive letter (C:/) or UNC (//)
[[nodiscard]]
auto is_absolute(std::string_view p) -> bool;

/// Lexically normalize a path by resolving '.' and '..' segments.
/// normalize("src/../include/./foo.h") → "include/foo.h"
/// normalize("/a/b/../c") → "/a/c"
/// Does not touch the filesystem.
[[nodiscard]]
auto normalize(std::string_view p) -> std::string;

/// Compute relative path from base to target (lexical, no filesystem access).
/// relative("a/b/c", "a") → "b/c"
/// relative("a/b", "a/b") → "."
/// relative("x/y", "a/b") → "../../x/y"
[[nodiscard]]
auto relative(std::string_view target, std::string_view base) -> std::string;

} // namespace path
} // namespace pup
