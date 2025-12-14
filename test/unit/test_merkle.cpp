// SPDX-License-Identifier: MIT
// Copyright (c) 2024 pup authors

#include "catch_amalgamated.hpp"
#include "pup/core/hash.hpp"
#include "pup/core/types.hpp"
#include "pup/index/entry.hpp"

#include <algorithm>
#include <random>
#include <tuple>
#include <vector>

using namespace pup;
using namespace pup::index;

// =============================================================================
// Helper Types
// =============================================================================

/// Statistics for tracking change detection operations
struct ChangeDetectionStats {
    std::size_t dir_hash_comparisons = 0;
    std::size_t file_hash_comparisons = 0;
    std::size_t dirs_scanned = 0;
    std::size_t files_checked = 0;
};

// =============================================================================
// Test Helpers
// =============================================================================

namespace {

/// Create a simple index with a directory structure for testing
auto create_simple_index() -> Index
{
    auto index = Index {};

    // src/
    //   main.c
    //   lib/
    //     util.c
    index.add_file({
        .id = 1,
        .parent_id = 0,
        .type = NodeType::Directory,
        .name = "src",
    });
    index.add_file({
        .id = 2,
        .parent_id = 1,
        .type = NodeType::File,
        .name = "main.c",
        .content_hash = sha256("main content"),
    });
    index.add_file({
        .id = 3,
        .parent_id = 1,
        .type = NodeType::Directory,
        .name = "lib",
    });
    index.add_file({
        .id = 4,
        .parent_id = 3,
        .type = NodeType::File,
        .name = "util.c",
        .content_hash = sha256("util content"),
    });

    return index;
}

/// Create a large index for complexity testing
auto create_large_index(std::size_t num_files, std::size_t num_dirs) -> Index
{
    auto index = Index {};
    auto next_id = NodeId { 1 };

    // Create directories
    auto dir_ids = std::vector<NodeId> {};
    dir_ids.push_back(0); // root

    for (std::size_t i = 0; i < num_dirs; ++i) {
        auto parent = dir_ids[i % dir_ids.size()];
        index.add_file({
            .id = next_id,
            .parent_id = parent,
            .type = NodeType::Directory,
            .name = "dir" + std::to_string(i),
        });
        dir_ids.push_back(next_id);
        ++next_id;
    }

    // Create files distributed across directories
    for (std::size_t i = 0; i < num_files; ++i) {
        auto parent = dir_ids[(i % (num_dirs - 1)) + 1]; // Skip root
        auto content = "file" + std::to_string(i);
        index.add_file({
            .id = next_id,
            .parent_id = parent,
            .type = NodeType::File,
            .name = "file" + std::to_string(i) + ".c",
            .content_hash = sha256(content),
        });
        ++next_id;
    }

    return index;
}

/// Create a deep index with specified depth
/// Files are placed only at the deepest level for O(log n) testing
auto create_deep_index(std::size_t depth, std::size_t files_per_dir) -> Index
{
    auto index = Index {};
    auto next_id = NodeId { 1 };

    // Create nested directories: d0/d1/d2/...
    auto parent = NodeId { 0 };
    auto deepest_dir_id = NodeId { 0 };

    for (std::size_t d = 0; d < depth; ++d) {
        index.add_file({
            .id = next_id,
            .parent_id = parent,
            .type = NodeType::Directory,
            .name = "d" + std::to_string(d),
        });
        parent = next_id;
        deepest_dir_id = next_id;
        ++next_id;
    }

    // Add files only to the deepest directory
    for (std::size_t f = 0; f < files_per_dir; ++f) {
        auto content = "content_" + std::to_string(deepest_dir_id) + "_" + std::to_string(f);
        index.add_file({
            .id = next_id,
            .parent_id = deepest_dir_id,
            .type = NodeType::File,
            .name = "file" + std::to_string(f) + ".c",
            .content_hash = sha256(content),
        });
        ++next_id;
    }

    return index;
}

/// Create an index with two subtrees (one to be changed, one unchanged)
auto create_two_subtree_index(std::size_t changed_files, std::size_t unchanged_files) -> Index
{
    auto index = Index {};
    auto next_id = NodeId { 1 };

    // src/
    index.add_file({
        .id = next_id++,
        .parent_id = 0,
        .type = NodeType::Directory,
        .name = "src",
    });
    auto src_id = NodeId { 1 };

    // src/changed/
    index.add_file({
        .id = next_id++,
        .parent_id = src_id,
        .type = NodeType::Directory,
        .name = "changed",
    });
    auto changed_id = NodeId { 2 };

    // src/unchanged/
    index.add_file({
        .id = next_id++,
        .parent_id = src_id,
        .type = NodeType::Directory,
        .name = "unchanged",
    });
    auto unchanged_id = NodeId { 3 };

    // Add files to changed/
    for (std::size_t i = 0; i < changed_files; ++i) {
        index.add_file({
            .id = next_id++,
            .parent_id = changed_id,
            .type = NodeType::File,
            .name = "file" + std::to_string(i) + ".c",
            .content_hash = sha256("changed_" + std::to_string(i)),
        });
    }

    // Add files to unchanged/
    for (std::size_t i = 0; i < unchanged_files; ++i) {
        index.add_file({
            .id = next_id++,
            .parent_id = unchanged_id,
            .type = NodeType::File,
            .name = "file" + std::to_string(i) + ".c",
            .content_hash = sha256("unchanged_" + std::to_string(i)),
        });
    }

    return index;
}

/// Copy an index (deep copy)
auto copy_index(Index const& src) -> Index
{
    auto dst = Index {};
    for (auto const& file : src.files()) {
        dst.add_file(file);
    }
    for (auto const& cmd : src.commands()) {
        dst.add_command(cmd);
    }
    for (auto const& edge : src.edges()) {
        dst.add_edge(edge);
    }
    return dst;
}

/// Modify a file's hash in an index (simulates file content change)
[[maybe_unused]] auto modify_file_hash(Index& index, std::string_view path) -> void
{
    for (auto& file : index.files()) {
        if (file.path == path || file.name == path) {
            file.content_hash = sha256("modified_" + std::string(path));
            return;
        }
    }
}

/// Find a random file path in the index
[[maybe_unused]] auto find_random_file(Index const& index) -> std::string
{
    for (auto const& file : index.files()) {
        if (file.type == NodeType::File) {
            return file.name;
        }
    }
    return "";
}

/// Find changed files using Merkle tree comparison (O(log n + k))
auto find_changed_files_merkle(
    Index const& old_index,
    Index const& new_index,
    ChangeDetectionStats* stats) -> std::vector<std::string>
{
    auto result = std::vector<std::string> {};

    if (stats) {
        stats->dir_hash_comparisons = 0;
        stats->file_hash_comparisons = 0;
        stats->dirs_scanned = 0;
        stats->files_checked = 0;
    }

    // Build id->file maps for O(1) lookups
    auto old_id_map = std::unordered_map<NodeId, FileEntry const*> {};
    for (auto const& file : old_index.files()) {
        old_id_map[file.id] = &file;
    }

    auto new_id_map = std::unordered_map<NodeId, FileEntry const*> {};
    for (auto const& file : new_index.files()) {
        new_id_map[file.id] = &file;
    }

    // Recursive Merkle comparison
    std::function<void(NodeId)> compare_subtree = [&](NodeId dir_id) {
        auto const* old_dir = old_id_map.count(dir_id) ? old_id_map.at(dir_id) : nullptr;
        auto const* new_dir = new_id_map.count(dir_id) ? new_id_map.at(dir_id) : nullptr;

        if (!new_dir)
            return;

        // Compare directory hashes (Merkle optimization)
        if (old_dir && stats) {
            stats->dir_hash_comparisons++;
        }

        if (old_dir && old_dir->content_hash == new_dir->content_hash) {
            // Hashes match - entire subtree is unchanged, skip it
            return;
        }

        // Hashes differ or no old dir - scan children
        if (stats) {
            stats->dirs_scanned++;
        }

        for (auto child_id : new_index.get_children(dir_id)) {
            auto const* new_child = new_id_map.count(child_id) ? new_id_map.at(child_id) : nullptr;
            if (!new_child)
                continue;

            if (new_child->type == NodeType::Directory || new_child->type == NodeType::GeneratedDir) {
                // Recurse into subdirectory
                compare_subtree(child_id);
            } else {
                // File - compare content hash
                if (stats) {
                    stats->files_checked++;
                }

                auto const* old_child = old_id_map.count(child_id) ? old_id_map.at(child_id) : nullptr;
                if (!old_child) {
                    result.push_back(new_child->name);
                    continue;
                }

                if (stats) {
                    stats->file_hash_comparisons++;
                }

                if (old_child->content_hash != new_child->content_hash) {
                    result.push_back(new_child->name);
                }
            }
        }
    };

    // Start from root children (parent_id == 0)
    for (auto root_child_id : new_index.get_children(0)) {
        auto const* root_child = new_id_map.count(root_child_id) ? new_id_map.at(root_child_id) : nullptr;
        if (!root_child)
            continue;

        if (root_child->type == NodeType::Directory || root_child->type == NodeType::GeneratedDir) {
            compare_subtree(root_child_id);
        } else {
            if (stats) {
                stats->files_checked++;
            }

            auto const* old_child = old_id_map.count(root_child_id) ? old_id_map.at(root_child_id) : nullptr;
            if (!old_child) {
                result.push_back(root_child->name);
                continue;
            }

            if (stats) {
                stats->file_hash_comparisons++;
            }

            if (old_child->content_hash != root_child->content_hash) {
                result.push_back(root_child->name);
            }
        }
    }

    return result;
}

/// Find changed files using legacy O(n) comparison
auto find_changed_files_legacy(
    Index const& old_index,
    Index const& new_index,
    ChangeDetectionStats* stats) -> std::vector<std::string>
{
    auto result = std::vector<std::string> {};

    if (stats) {
        stats->files_checked = 0;
        stats->file_hash_comparisons = 0;
    }

    // O(n) - check every file
    for (auto const& new_file : new_index.files()) {
        if (new_file.type != NodeType::File && new_file.type != NodeType::Generated) {
            continue;
        }

        if (stats) {
            stats->files_checked++;
        }

        auto const* old_file = old_index.find_file_by_id(new_file.id);
        if (!old_file) {
            result.push_back(new_file.name);
            continue;
        }

        if (stats) {
            stats->file_hash_comparisons++;
        }

        if (old_file->content_hash != new_file.content_hash) {
            result.push_back(new_file.name);
        }
    }

    return result;
}

} // namespace

// =============================================================================
// Merkle Hash Computation Tests
// =============================================================================

TEST_CASE("Merkle hash computation", "[merkle]")
{
    SECTION("empty directory has deterministic hash")
    {
        auto children = std::vector<std::tuple<std::string_view, NodeType, Hash256 const*>> {};
        auto hash1 = compute_merkle_hash(children);
        auto hash2 = compute_merkle_hash(children);

        CHECK(hash1 != ZERO_HASH);
        CHECK(hash1 == hash2);
    }

    SECTION("hash changes when child added")
    {
        auto file_hash = sha256("content");
        auto children_empty = std::vector<std::tuple<std::string_view, NodeType, Hash256 const*>> {};
        auto children_one = std::vector<std::tuple<std::string_view, NodeType, Hash256 const*>> {
            { "file.c", NodeType::File, &file_hash }
        };

        auto hash_empty = compute_merkle_hash(children_empty);
        auto hash_one = compute_merkle_hash(children_one);

        CHECK(hash_empty != hash_one);
    }

    SECTION("hash changes when child content changes")
    {
        auto hash_v1 = sha256("version1");
        auto hash_v2 = sha256("version2");

        auto children_v1 = std::vector<std::tuple<std::string_view, NodeType, Hash256 const*>> {
            { "file.c", NodeType::File, &hash_v1 }
        };
        auto children_v2 = std::vector<std::tuple<std::string_view, NodeType, Hash256 const*>> {
            { "file.c", NodeType::File, &hash_v2 }
        };

        CHECK(compute_merkle_hash(children_v1) != compute_merkle_hash(children_v2));
    }

    SECTION("hash changes when child renamed")
    {
        auto file_hash = sha256("content");

        auto children_a = std::vector<std::tuple<std::string_view, NodeType, Hash256 const*>> {
            { "a.c", NodeType::File, &file_hash }
        };
        auto children_b = std::vector<std::tuple<std::string_view, NodeType, Hash256 const*>> {
            { "b.c", NodeType::File, &file_hash }
        };

        CHECK(compute_merkle_hash(children_a) != compute_merkle_hash(children_b));
    }

    SECTION("ordering is deterministic regardless of input order")
    {
        auto hash_a = sha256("a");
        auto hash_b = sha256("b");

        auto order1 = std::vector<std::tuple<std::string_view, NodeType, Hash256 const*>> {
            { "a.c", NodeType::File, &hash_a },
            { "b.c", NodeType::File, &hash_b }
        };
        auto order2 = std::vector<std::tuple<std::string_view, NodeType, Hash256 const*>> {
            { "b.c", NodeType::File, &hash_b },
            { "a.c", NodeType::File, &hash_a }
        };

        CHECK(compute_merkle_hash(order1) == compute_merkle_hash(order2));
    }

    SECTION("hash includes node type")
    {
        auto content_hash = sha256("content");

        auto as_file = std::vector<std::tuple<std::string_view, NodeType, Hash256 const*>> {
            { "foo", NodeType::File, &content_hash }
        };
        auto as_dir = std::vector<std::tuple<std::string_view, NodeType, Hash256 const*>> {
            { "foo", NodeType::Directory, &content_hash }
        };

        CHECK(compute_merkle_hash(as_file) != compute_merkle_hash(as_dir));
    }
}

// =============================================================================
// Children Index Tests
// =============================================================================

TEST_CASE("Children index", "[merkle]")
{
    SECTION("build_children_index creates correct parent->children mapping")
    {
        auto index = Index {};
        index.add_file({ .id = 1, .parent_id = 0, .type = NodeType::Directory, .name = "src" });
        index.add_file({ .id = 2, .parent_id = 1, .type = NodeType::File, .name = "main.c" });
        index.add_file({ .id = 3, .parent_id = 1, .type = NodeType::File, .name = "util.c" });

        index.build_children_index();

        auto children = index.get_children(1);
        CHECK(children.size() == 2);
        CHECK(std::find(children.begin(), children.end(), NodeId { 2 }) != children.end());
        CHECK(std::find(children.begin(), children.end(), NodeId { 3 }) != children.end());
    }

    SECTION("get_children returns empty for leaf directory")
    {
        auto index = Index {};
        index.add_file({ .id = 1, .parent_id = 0, .type = NodeType::Directory, .name = "empty" });

        index.build_children_index();

        CHECK(index.get_children(1).empty());
    }

    SECTION("get_children returns empty for files")
    {
        auto index = Index {};
        index.add_file({ .id = 1, .parent_id = 0, .type = NodeType::File, .name = "file.c" });

        index.build_children_index();

        CHECK(index.get_children(1).empty());
    }

    SECTION("root children are accessible")
    {
        auto index = Index {};
        index.add_file({ .id = 1, .parent_id = 0, .type = NodeType::Directory, .name = "src" });
        index.add_file({ .id = 2, .parent_id = 0, .type = NodeType::Directory, .name = "include" });

        index.build_children_index();

        auto root_children = index.get_children(0);
        CHECK(root_children.size() == 2);
    }
}

// =============================================================================
// Bottom-up Merkle Hash Computation Tests
// =============================================================================

TEST_CASE("Bottom-up Merkle hash computation", "[merkle]")
{
    SECTION("nested directories compute correctly")
    {
        auto index = create_simple_index();

        index.build_children_index();
        index.compute_merkle_hashes();

        // lib/ has hash computed from util.c
        auto const* lib = index.find_file_by_id(3);
        REQUIRE(lib != nullptr);
        CHECK(lib->content_hash != ZERO_HASH);

        // src/ has hash computed from lib/ and main.c
        auto const* src = index.find_file_by_id(1);
        REQUIRE(src != nullptr);
        CHECK(src->content_hash != ZERO_HASH);
        CHECK(src->content_hash != lib->content_hash);
    }

    SECTION("file hashes are preserved")
    {
        auto index = create_simple_index();
        auto original_main_hash = index.find_file_by_id(2)->content_hash;

        index.build_children_index();
        index.compute_merkle_hashes();

        // File hash should not change
        CHECK(index.find_file_by_id(2)->content_hash == original_main_hash);
    }

    SECTION("identical trees have identical root hash")
    {
        auto index1 = create_simple_index();
        auto index2 = create_simple_index();

        index1.build_children_index();
        index1.compute_merkle_hashes();

        index2.build_children_index();
        index2.compute_merkle_hashes();

        // Root directories should have same hash
        CHECK(index1.find_file_by_id(1)->content_hash == index2.find_file_by_id(1)->content_hash);
    }

    SECTION("changing one file changes ancestor hashes")
    {
        auto index1 = create_simple_index();
        auto index2 = copy_index(index1);

        // Modify util.c in index2
        for (auto& file : index2.files()) {
            if (file.name == "util.c") {
                file.content_hash = sha256("modified");
                break;
            }
        }

        index1.build_children_index();
        index1.compute_merkle_hashes();

        index2.build_children_index();
        index2.compute_merkle_hashes();

        // lib/ hash should differ (direct parent of util.c)
        CHECK(index1.find_file_by_id(3)->content_hash != index2.find_file_by_id(3)->content_hash);

        // src/ hash should also differ (ancestor)
        CHECK(index1.find_file_by_id(1)->content_hash != index2.find_file_by_id(1)->content_hash);
    }
}

// =============================================================================
// Complexity Verification Tests
// =============================================================================

TEST_CASE("Merkle detection complexity", "[merkle][complexity]")
{
    SECTION("nothing changed - O(1) operations")
    {
        auto index = create_large_index(1000, 100);
        index.build_children_index();
        index.compute_merkle_hashes();

        auto stats = ChangeDetectionStats {};
        auto changed = find_changed_files_merkle(index, index, &stats);

        CHECK(changed.empty());
        CHECK(stats.dir_hash_comparisons == 1); // Only root
        CHECK(stats.file_hash_comparisons == 0);
        CHECK(stats.dirs_scanned == 0);
        CHECK(stats.files_checked == 0);
    }

    SECTION("single file change - O(log n) operations")
    {
        auto old_index = create_deep_index(5, 200); // ~1000 files
        auto new_index = copy_index(old_index);

        // Modify a file in the deepest directory
        for (auto& file : new_index.files()) {
            if (file.type == NodeType::File && file.parent_id == 5) {
                file.content_hash = sha256("modified");
                break;
            }
        }

        old_index.build_children_index();
        old_index.compute_merkle_hashes();
        new_index.build_children_index();
        new_index.compute_merkle_hashes();

        auto stats = ChangeDetectionStats {};
        auto changed = find_changed_files_merkle(old_index, new_index, &stats);

        CHECK(changed.size() == 1);
        CHECK(stats.dirs_scanned <= 5); // Only path to change
        CHECK(stats.files_checked < 500); // Far less than 1000
    }

    SECTION("unchanged subtree is skipped entirely")
    {
        auto old_index = create_two_subtree_index(10, 990);
        auto new_index = copy_index(old_index);

        // Modify a file in src/changed/
        for (auto& file : new_index.files()) {
            if (file.type == NodeType::File && file.parent_id == 2) {
                file.content_hash = sha256("modified");
                break;
            }
        }

        old_index.build_children_index();
        old_index.compute_merkle_hashes();
        new_index.build_children_index();
        new_index.compute_merkle_hashes();

        auto stats = ChangeDetectionStats {};
        auto changed = find_changed_files_merkle(old_index, new_index, &stats);

        // unchanged/ subtree should be completely skipped
        CHECK(stats.files_checked <= 15);

        auto skip_ratio = 1.0 - (double(stats.files_checked) / 1000.0);
        CHECK(skip_ratio > 0.98);
    }

    SECTION("compare to legacy O(n) - quantitative improvement")
    {
        auto old_index = create_large_index(1000, 100);
        auto new_index = copy_index(old_index);

        // Modify one file
        for (auto& file : new_index.files()) {
            if (file.type == NodeType::File) {
                file.content_hash = sha256("modified");
                break;
            }
        }

        old_index.build_children_index();
        old_index.compute_merkle_hashes();
        new_index.build_children_index();
        new_index.compute_merkle_hashes();

        auto merkle_stats = ChangeDetectionStats {};
        auto legacy_stats = ChangeDetectionStats {};

        find_changed_files_merkle(old_index, new_index, &merkle_stats);
        find_changed_files_legacy(old_index, new_index, &legacy_stats);

        CHECK(legacy_stats.files_checked == 1000);
        CHECK(merkle_stats.files_checked < 100);

        auto improvement = double(legacy_stats.files_checked) / double(merkle_stats.files_checked);
        CHECK(improvement > 10);
    }
}

TEST_CASE("Complexity scales logarithmically", "[merkle][complexity]")
{
    auto run_test = [](std::size_t n) -> std::size_t {
        auto old_index = create_large_index(n, n / 10);
        auto new_index = copy_index(old_index);

        for (auto& file : new_index.files()) {
            if (file.type == NodeType::File) {
                file.content_hash = sha256("modified");
                break;
            }
        }

        old_index.build_children_index();
        old_index.compute_merkle_hashes();
        new_index.build_children_index();
        new_index.compute_merkle_hashes();

        auto stats = ChangeDetectionStats {};
        find_changed_files_merkle(old_index, new_index, &stats);
        return stats.files_checked;
    };

    auto checks_1000 = run_test(1000);
    auto checks_2000 = run_test(2000);
    auto checks_4000 = run_test(4000);

    // Verify sublinear scaling
    CHECK(checks_2000 < checks_1000 * 1.5);
    CHECK(checks_4000 < checks_1000 * 2.0);
}

// =============================================================================
// Algorithm Correctness Tests
// =============================================================================

TEST_CASE("Merkle change detection correctness", "[merkle]")
{
    SECTION("identical indexes return no changes")
    {
        auto index = create_simple_index();
        index.build_children_index();
        index.compute_merkle_hashes();

        auto changed = find_changed_files_merkle(index, index, nullptr);
        CHECK(changed.empty());
    }

    SECTION("single file change detected")
    {
        auto old_index = create_simple_index();
        auto new_index = copy_index(old_index);

        for (auto& file : new_index.files()) {
            if (file.name == "main.c") {
                file.content_hash = sha256("modified");
                break;
            }
        }

        old_index.build_children_index();
        old_index.compute_merkle_hashes();
        new_index.build_children_index();
        new_index.compute_merkle_hashes();

        auto changed = find_changed_files_merkle(old_index, new_index, nullptr);
        CHECK(changed.size() == 1);
        CHECK(changed[0] == "main.c");
    }

    SECTION("results match legacy O(n) detection")
    {
        auto old_index = create_large_index(100, 10);
        auto new_index = copy_index(old_index);

        // Make a few random changes
        auto count = 0;
        for (auto& file : new_index.files()) {
            if (file.type == NodeType::File && count < 5) {
                file.content_hash = sha256("modified_" + std::to_string(count));
                ++count;
            }
        }

        old_index.build_children_index();
        old_index.compute_merkle_hashes();
        new_index.build_children_index();
        new_index.compute_merkle_hashes();

        auto merkle_result = find_changed_files_merkle(old_index, new_index, nullptr);
        auto legacy_result = find_changed_files_legacy(old_index, new_index, nullptr);

        std::sort(merkle_result.begin(), merkle_result.end());
        std::sort(legacy_result.begin(), legacy_result.end());

        CHECK(merkle_result == legacy_result);
    }
}
