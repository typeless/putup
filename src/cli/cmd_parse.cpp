// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/cli/commands.hpp"
#include "pup/cli/context.hpp"
#include "pup/cli/multi_variant.hpp"
#include "pup/cli/options.hpp"
#include "pup/cli/strict_checks.hpp"
#include "pup/core/global_pool.hpp"
#include "pup/core/layout.hpp"
#include "pup/core/path.hpp"
#include "pup/core/print.hpp"
#include "pup/core/result.hpp"
#include "pup/core/string_pool.hpp"
#include "pup/core/types.hpp"
#include "pup/core/vec.hpp"
#include "pup/graph/dag.hpp"
#include "pup/parser/ast.hpp"
#include "pup/platform/file_io.hpp"

#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <utility>

namespace pup::cli {

namespace {

auto parse_single_variant(Options const& opts, std::string_view variant_name) -> int
{
    auto& pool = global_pool();
    auto layout = discover_layout(make_layout_options(opts));
    if (!layout) {
        eprint("[{}] Error: {}\n", variant_name, layout.error().msg());
        return EXIT_FAILURE;
    }

    auto diagnostics = Vec<Diagnostic> {};

    auto ctx_opts = BuildContextOptions {
        .verbose = opts.verbose,
        .parse_scopes = compute_build_scopes(opts, *layout),
    };

    if (opts.check != CheckLevel::None) {
        auto source_root_sv = pool.get(layout->source_root);
        auto config_root_sv = pool.get(layout->config_root);

        ctx_opts.on_statement = [&diagnostics, source_root_sv, config_root_sv](
                                    parser::Statement const& stmt,
                                    std::string_view /*dir*/
                                ) {
            auto const* assign = stmt.as<parser::Assignment>();
            if (!assign) {
                return;
            }

            auto file_sv = stmt.location.filename;
            auto file_dir = pup::path::parent(file_sv);
            auto is_tree_root = file_dir == source_root_sv || file_dir == config_root_sv;
            auto is_component = !file_dir.empty() && file_dir != "." && !is_tree_root;
            auto diags = check_assignment(*assign, file_sv, is_component);
            for (auto& d : diags) {
                diagnostics.push_back(std::move(d));
            }
        };
    }

    auto result = pup::Result<BuildContext> { build_context(opts, ctx_opts) };
    if (!result) {
        eprint("[{}] Error: {}\n", variant_name, result.error().msg());
        return EXIT_FAILURE;
    }

    auto& ctx = *result;

    if (opts.verbose) {
        print("[{}] Project root: \"{}\"\n", variant_name, pool.get(ctx.layout().source_root));
        print("[{}] Tupfiles:\n", variant_name);
        for (auto dir_id : ctx.parsed_dirs()) {
            auto dir_sv = pool.get(dir_id);
            auto tupfile_path = (dir_sv == "." || dir_sv.empty())
                ? pool.get(pup::path::join(pool.get(ctx.layout().source_root), "Tupfile"))
                : pool.get(pup::path::join(pool.get(pup::path::join(pool.get(ctx.layout().source_root), dir_sv)), "Tupfile"));
            print("[{}]   {}\n", variant_name, tupfile_path);
        }
    }

    auto commands = pup::graph::nodes_of_type(ctx.graph().graph, pup::NodeType::Command);

    if (opts.verbose && !commands.empty()) {
        print("[{}] Commands:\n", variant_name);
        auto cache = pup::graph::PathCache {};
        for (auto id : commands) {
            auto label = pool.get(
                pup::graph::command_label(ctx.graph().graph, id, cache, pool.get(ctx.layout().source_root), pool.get(ctx.layout().config_root))
            );
            print("[{}]   {}\n", variant_name, label);
        }
    }

    print("[{}] Parsed {} Tupfile(s), {} commands\n", variant_name, ctx.parsed_dirs().size(), commands.size());

    if (opts.check != CheckLevel::None) {
        auto component_dirs = Vec<std::string_view> {};
        for (auto dir_id : ctx.parsed_dirs()) {
            auto dir_sv = pool.get(dir_id);
            if (dir_sv.empty() || dir_sv == ".") {
                continue;
            }
            auto tuprules_path = pool.get(pup::path::join(
                pool.get(pup::path::join(pool.get(ctx.layout().source_root), dir_sv)),
                "Tuprules.tup"
            ));
            if (pup::platform::exists(tuprules_path)) {
                component_dirs.push_back(
                    pool.get(pup::path::join(pool.get(ctx.layout().source_root), dir_sv))
                );
            }
        }

        auto fs_diags = check_component_dirs(component_dirs);
        for (auto& d : fs_diags) {
            diagnostics.push_back(std::move(d));
        }

        auto scan_diags = check_unscanned_compiles(ctx.graph().graph, ctx.graph().path_cache);
        for (auto& d : scan_diags) {
            diagnostics.push_back(std::move(d));
        }

        auto has_errors = false;
        for (auto const& d : diagnostics) {
            auto severity_str = d.severity == Diagnostic::Error ? "error" : "warning";
            eprint("{}:{}: {}: {}\n", pool.get(d.file), d.line, severity_str, pool.get(d.message));
            if (d.severity == Diagnostic::Error) {
                has_errors = true;
            }
        }

        if (has_errors && opts.check == CheckLevel::Error) {
            return EXIT_FAILURE;
        }
    }

    return EXIT_SUCCESS;
}

} // namespace

auto cmd_parse(Options const& opts) -> int
{
    return for_each_variant(opts, parse_single_variant, "Parsing");
}

} // namespace pup::cli
