// SPDX-License-Identifier: MIT
// Copyright (c) 2024 pup authors

#include "pup/cli/commands.hpp"
#include "pup/cli/options.hpp"

#include <cstdlib>
#include <exception>

#include <fmt/core.h>

namespace pup::cli {

auto dispatch(Options const& opts) -> int
{
    if (opts.command.empty()) {
        return cmd_build(opts);
    }
    if (opts.command == "init") {
        return cmd_init(opts);
    }
    if (opts.command == "parse") {
        return cmd_parse(opts);
    }
    if (opts.command == "export") {
        return cmd_export(opts);
    }
    if (opts.command == "clean") {
        return cmd_clean(opts);
    }
    if (opts.command == "distclean") {
        return cmd_distclean(opts);
    }
    if (opts.command == "variant") {
        return cmd_variant(opts);
    }

    fmt::print(stderr, "Unknown command: {}\n", opts.command);
    print_usage();
    return EXIT_FAILURE;
}

} // namespace pup::cli

auto main(int argc, char** argv) noexcept(false) -> int
{
    try {
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
    } catch (std::exception const& e) {
        fmt::print(stderr, "Fatal error: {}\n", e.what());
        return EXIT_FAILURE;
    } catch (...) {
        fmt::print(stderr, "Fatal error: unknown exception\n");
        return EXIT_FAILURE;
    }
}
