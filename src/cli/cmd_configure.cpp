// SPDX-License-Identifier: MIT
// Copyright (c) 2024 pup authors

#include "pup/cli/commands.hpp"

namespace pup::cli {

auto cmd_configure(Options const& opts) -> int
{
    // Configure is implemented as a build mode - cmd_build handles it
    // when opts.command == "configure"
    return cmd_build(opts);
}

} // namespace pup::cli
