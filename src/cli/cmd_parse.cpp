// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/cli/commands.hpp"
#include "pup/cli/context.hpp"
#include "pup/cli/multi_variant.hpp"
#include "pup/core/layout.hpp"
#include "pup/core/types.hpp"
#include "pup/graph/dag.hpp"

#include <cstdio>
#include <cstdlib>

namespace pup::cli {

namespace {

auto parse_single_variant(Options const& opts, std::string_view variant_name) -> int
{
    auto ctx_opts = BuildContextOptions {
        .verbose = opts.verbose,
    };

    auto result = pup::Result<BuildContext> { build_context(opts, ctx_opts) };
    if (!result) {
        fprintf(stderr, "[%.*s] Error: %s\n", static_cast<int>(variant_name.size()), variant_name.data(), result.error().message.c_str());
        return EXIT_FAILURE;
    }

    auto& ctx = *result;

    if (opts.verbose) {
        printf("[%.*s] Project root: \"%s\"\n", static_cast<int>(variant_name.size()), variant_name.data(), ctx.layout().source_root.string().c_str());
        printf("[%.*s] Tupfiles:\n", static_cast<int>(variant_name.size()), variant_name.data());
        for (auto const& dir : ctx.parsed_dirs()) {
            auto tupfile_path = (dir == "." || dir.empty())
                ? ctx.layout().source_root / "Tupfile"
                : ctx.layout().source_root / dir / "Tupfile";
            printf("[%.*s]   %s\n", static_cast<int>(variant_name.size()), variant_name.data(), tupfile_path.string().c_str());
        }
    }

    auto commands = ctx.graph().nodes_of_type(pup::NodeType::Command);

    if (opts.verbose && !commands.empty()) {
        printf("[%.*s] Commands:\n", static_cast<int>(variant_name.size()), variant_name.data());
        for (auto id : commands) {
            if (auto const* node = ctx.graph().get_node(id)) {
                printf("[%.*s]   %s\n", static_cast<int>(variant_name.size()), variant_name.data(), (node->display.empty() ? node->command : node->display).c_str());
            }
        }
    }

    printf("[%.*s] Parsed %zu Tupfile(s), %zu commands\n", static_cast<int>(variant_name.size()), variant_name.data(), ctx.parsed_dirs().size(), commands.size());

    return EXIT_SUCCESS;
}

} // namespace

auto cmd_parse(Options const& opts) -> int
{
    return for_each_variant(opts, parse_single_variant, "Parsing");
}

} // namespace pup::cli
