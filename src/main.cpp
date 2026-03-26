// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/cli/commands.hpp"
#include "pup/cli/options.hpp"
#include "pup/core/global_pool.hpp"
#include "pup/core/string_pool.hpp"

#include <cstdio>
#include <cstdlib>

namespace pup::cli {

auto dispatch(Options const& opts) -> int
{
    auto const& pool = global_pool();
    auto cmd = pool.get(opts.command);

    if (cmd.empty()) {
        return cmd_build(opts);
    }
    if (cmd == "parse") {
        return cmd_parse(opts);
    }
    if (cmd == "show") {
        return cmd_show(opts);
    }
    if (cmd == "clean") {
        return cmd_clean(opts);
    }
    if (cmd == "distclean") {
        return cmd_distclean(opts);
    }
    if (cmd == "configure") {
        return cmd_configure(opts);
    }

    fprintf(stderr, "Unknown command: %s\n", cmd.data());
    print_usage();
    return EXIT_FAILURE;
}

} // namespace pup::cli

auto main(int argc, char** argv) -> int
{
    auto opts = pup::cli::Options { pup::cli::parse_args(argc, argv) };

    if (opts.help) {
        pup::cli::print_usage();
        return EXIT_SUCCESS;
    }

    if (opts.version) {
        pup::cli::print_version();
        return EXIT_SUCCESS;
    }

    return pup::cli::dispatch(opts);
}
