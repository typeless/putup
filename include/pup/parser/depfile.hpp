// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include "pup/core/result.hpp"
#include "pup/core/string_id.hpp"
#include "pup/core/vec.hpp"

#include <string_view>

namespace pup::parser {

/// Parsed dependency file (.d file from gcc -MD)
struct Depfile {
    StringId target = StringId::Empty; ///< Output file (left side of colon)
    Vec<StringId> dependencies;        ///< Input files (right side of colon)
};

/// Parse a dependency file from a filesystem path
[[nodiscard]]
auto parse_depfile_path(std::string_view path) -> Result<Depfile>;

/// Parse a dependency file from string content
[[nodiscard]]
auto parse_depfile(std::string_view content) -> Result<Depfile>;

/// Whether a parse is the rule a dep scan asked for, rather than driver chatter that happens to
/// carry a colon. A scan names its target after the object, so anything else on stdout -- a
/// `/showIncludes` note, a banner -- parses into dependencies that never stat and re-trigger for
/// ever, which is why the scan is rejected instead of recorded.
[[nodiscard]]
auto is_scan_output(Depfile const& depfile) -> bool;

} // namespace pup::parser
