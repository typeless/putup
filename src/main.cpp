// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/cli/commands.hpp"
#include "pup/cli/options.hpp"

#include <cstdio>
#include <cstdlib>

namespace pup::cli {

auto dispatch(Options const& opts) -> int
{
    if (opts.command.empty()) {
        return cmd_build(opts);
    }
    if (opts.command == "parse") {
        return cmd_parse(opts);
    }
    if (opts.command == "show") {
        return cmd_show(opts);
    }
    if (opts.command == "clean") {
        return cmd_clean(opts);
    }
    if (opts.command == "distclean") {
        return cmd_distclean(opts);
    }
    if (opts.command == "configure") {
        return cmd_configure(opts);
    }

    fprintf(stderr, "Unknown command: %s\n", opts.command.c_str());
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
