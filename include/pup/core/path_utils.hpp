// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include "pup/core/string_id.hpp"
#include "pup/core/vec.hpp"

#include <optional>
#include <string_view>

namespace pup {

/// Whether `path` names `root` or something inside it, by prefix.
///
/// Both must be normal -- no `.` or `..` component -- and expressed the same way, both absolute
/// or both relative to the same base. A `..` in `path` reads as an ordinary component here, so
/// `<root>/dir/../x`, which is outside `root`, answers yes; that is how #312's stray `..` became
/// a dropped dependency. Producers hold this: every caller passes `canonical`, `normalize` or
/// `path::relative` output. Asserted in debug builds.
[[nodiscard]]
auto is_path_under(
    std::string_view path,
    std::string_view root
) -> bool;

[[nodiscard]]
auto relative_to_root(
    std::string_view path,
    std::string_view root
) -> StringId;

[[nodiscard]]
auto is_path_in_scope(
    std::string_view path,
    std::string_view scope
) -> bool;

[[nodiscard]]
auto is_path_in_any_scope(
    std::string_view path,
    Vec<StringId> const& scopes
) -> bool;

[[nodiscard]]
auto compute_source_to_root(std::string_view source_dir) -> StringId;

/// Drop `prefix/` from the front of `path`, or return `path` unchanged when it does not lead with
/// it. Textual: `path` must be normal, or a `..` component makes the prefix mean something the
/// path does not. Asserted in debug builds.
[[nodiscard]]
auto strip_path_prefix(
    std::string_view path,
    std::string_view prefix
) -> StringId;

[[nodiscard]]
auto resolve_under_root(
    std::string_view path,
    std::string_view source_root,
    std::string_view target_root
) -> std::optional<StringId>;

[[nodiscard]]
auto make_source_relative(
    std::string_view path,
    std::string_view source_to_root,
    std::string_view source_dir
) -> StringId;

} // namespace pup
