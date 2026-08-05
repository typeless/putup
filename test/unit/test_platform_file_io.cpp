// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "catch_amalgamated.hpp"
#include "e2e_fixture.hpp"
#include "pup/core/global_pool.hpp"
#include "pup/core/path.hpp"
#include "pup/core/string_pool.hpp"
#include "pup/platform/file_io.hpp"

#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <pthread.h>
#endif

using namespace pup::platform;
using namespace pup::test;

SCENARIO("MappedFile provides read-only access to file contents", "[e2e][platform][file_io]")
{
    GIVEN("a file with known contents")
    {
        auto f = E2EFixture { "simple_c" };
        auto test_content = std::string { "Hello, World!" };
        f.write_file("test.bin", test_content);

        WHEN("the file is memory-mapped")
        {
            auto result = MappedFile::open((f.workdir() / "test.bin").string());

            THEN("the operation succeeds")
            {
                REQUIRE(result.has_value());
            }

            THEN("the mapped file is open")
            {
                REQUIRE(result->is_open());
            }

            THEN("the size matches the file content")
            {
                REQUIRE(result->size() == test_content.size());
            }

            THEN("the contents can be read correctly")
            {
                REQUIRE(std::memcmp(result->data(), test_content.data(), test_content.size()) == 0);
            }
        }
    }
}

SCENARIO("MappedFile handles missing files", "[platform][file_io]")
{
    GIVEN("a path to a non-existent file")
    {
        auto path = std::string { "/tmp/pup_test_nonexistent_12345.bin" };

        WHEN("attempting to memory-map the file")
        {
            auto result = MappedFile::open(path);

            THEN("the operation fails with an error")
            {
                REQUIRE_FALSE(result.has_value());
                REQUIRE(result.error().code == pup::ErrorCode::IoError);
            }
        }
    }
}

SCENARIO("MappedFile is move-only", "[e2e][platform][file_io]")
{
    GIVEN("a memory-mapped file")
    {
        auto f = E2EFixture { "simple_c" };
        f.write_file("test.bin", "test data");

        auto original = MappedFile::open((f.workdir() / "test.bin").string());
        REQUIRE(original.has_value());
        auto original_data = original->data();
        auto original_size = original->size();

        WHEN("moved to another MappedFile")
        {
            auto moved = MappedFile { std::move(*original) };

            THEN("the moved-from object is no longer open")
            {
                REQUIRE_FALSE(original->is_open());
            }

            THEN("the moved-to object has the same data")
            {
                REQUIRE(moved.is_open());
                REQUIRE(moved.data() == original_data);
                REQUIRE(moved.size() == original_size);
            }
        }
    }
}

TEST_CASE("stat_file returns file metadata", "[e2e][platform][file_io]")
{
    auto f = E2EFixture { "simple_c" };
    auto content = std::string { "test content with known size" };
    f.write_file("stat_test.txt", content);

    SECTION("returns correct size for existing file")
    {
        auto result = stat_file((f.workdir() / "stat_test.txt").string());
        REQUIRE(result.has_value());
        REQUIRE(result->size == content.size());
    }

    SECTION("returns error for non-existent file")
    {
        auto result = stat_file((f.workdir() / "nonexistent.txt").string());
        REQUIRE_FALSE(result.has_value());
        REQUIRE(result.error().code == pup::ErrorCode::IoError);
    }
}

TEST_CASE("atomic_write creates file atomically", "[e2e][platform][file_io]")
{
    auto f = E2EFixture { "simple_c" };
    auto content = std::string { "atomic write test content" };
    auto content_bytes = std::span<std::byte const> {
        reinterpret_cast<std::byte const*>(content.data()),
        content.size()
    };

    SECTION("creates new file with correct contents")
    {
        auto path = (f.workdir() / "atomic_test.txt").string();

        auto result = atomic_write(path, content_bytes);
        REQUIRE(result.has_value());

        auto written = f.read_file("atomic_test.txt");
        REQUIRE(written == content);
    }

    SECTION("overwrites existing file")
    {
        f.write_file("existing.txt", "old content");
        auto path = (f.workdir() / "existing.txt").string();

        auto result = atomic_write(path, content_bytes);
        REQUIRE(result.has_value());

        auto written = f.read_file("existing.txt");
        REQUIRE(written == content);
    }

    SECTION("creates parent directories if needed")
    {
        auto path = (f.workdir() / "subdir" / "nested" / "atomic_test.txt").string();

        auto result = atomic_write(path, content_bytes);
        REQUIRE(result.has_value());

        REQUIRE(std::filesystem::exists(path));
    }
}

namespace {

// A throwaway temp directory. These tests exercise the platform filesystem
// layer directly, so they must not pull in the E2E fixture (which resolves the
// putup binary and aborts when it is absent, e.g. on the Windows test runner).
class TempDir {
public:
    explicit TempDir(std::string_view name)
        : m_path { std::filesystem::temp_directory_path() / (std::string { "pup_walk_" } + std::string { name }) }
    {
        auto ec = std::error_code {};
        std::filesystem::remove_all(m_path, ec);
        std::filesystem::create_directories(m_path);
    }

    ~TempDir()
    {
        auto ec = std::error_code {};
        std::filesystem::remove_all(m_path, ec);
    }

    TempDir(TempDir const&) = delete;
    auto operator=(TempDir const&) -> TempDir& = delete;

    [[nodiscard]] auto path() const -> std::filesystem::path const& { return m_path; }

    auto write(std::string_view rel, std::string_view content) const -> void
    {
        auto p = m_path / rel;
        std::filesystem::create_directories(p.parent_path());
        auto ofs = std::ofstream { p };
        ofs << content;
    }

private:
    std::filesystem::path m_path;
};

// Build a fixed tree for the discovery tests:
//   a.txt      .hidden
//   sub/b.txt  sub/deep/c.txt
//   skip/d.txt   (skip/ is pruned by the walk visitor)
auto make_discovery_tree(TempDir const& dir) -> std::string
{
    dir.write("a.txt", "a");
    dir.write(".hidden", "h");
    dir.write("sub/b.txt", "b");
    dir.write("sub/deep/c.txt", "c");
    dir.write("skip/d.txt", "d");
    return dir.path().string();
}

} // namespace

SCENARIO("walk_directory reports every entry once with its relative path", "[platform][file_io][walk]")
{
    GIVEN("a nested directory tree with a pruned subtree")
    {
        auto dir = TempDir { "reports" };
        auto root = make_discovery_tree(dir);

        WHEN("the tree is walked, pruning the 'skip' directory")
        {
            auto seen = std::set<std::pair<std::string, bool>> {};
            auto r = pup::platform::walk_directory(
                root,
                [&](pup::platform::DirEntry const& entry, std::string_view rel_path) -> bool {
                    seen.insert({ std::string { rel_path }, entry.is_dir });
                    return !(entry.is_dir && rel_path == "skip");
                });

            THEN("the walk succeeds")
            {
                REQUIRE(r.has_value());
            }

            THEN("exactly the expected entries are visited")
            {
                auto expected = std::set<std::pair<std::string, bool>> {
                    { "a.txt", false },
                    { ".hidden", false },
                    { "sub", true },
                    { "sub/b.txt", false },
                    { "sub/deep", true },
                    { "sub/deep/c.txt", false },
                    { "skip", true },
                };
                REQUIRE(seen == expected);
            }

            THEN("the pruned subtree is not descended")
            {
                REQUIRE(seen.find({ "skip/d.txt", false }) == seen.end());
            }
        }
    }
}

TEST_CASE("walk_directory interns no per-entry strings", "[platform][file_io][walk]")
{
    // Names carry a prefix unique to this test so the walk sees a cold string
    // pool: every basename and relative path is genuinely new, so any per-entry
    // interning shows up as pool growth. Run once (TEST_CASE, no sections) —
    // Catch re-runs a SCENARIO's WHEN per THEN, which would warm the pool.
    auto dir = TempDir { "intern" };
    auto entry_count = 0;
    for (auto d = 0; d < 8; ++d) {
        for (auto i = 0; i < 8; ++i) {
            dir.write("pgzwalk_" + std::to_string(d) + "/pgzwalk_" + std::to_string(d) + "_" + std::to_string(i) + ".txt", "x");
            ++entry_count;
        }
        ++entry_count;
    }
    auto root = dir.path().string();

    auto& pool = pup::global_pool();
    auto before = pool.size();
    auto count = 0;
    (void)pup::platform::walk_directory(
        root,
        [&](pup::platform::DirEntry const& /*entry*/, std::string_view /*rel_path*/) -> bool {
            ++count;
            return true;
        });
    auto after = pool.size();

    INFO("before=" << before << " after=" << after << " entries=" << entry_count);
    REQUIRE(count == entry_count);
    REQUIRE(after == before);
}

#ifndef _WIN32
SCENARIO("A failed removal reports the reason the system gave", "[platform][file_io]")
{
    GIVEN("a file inside a directory that cannot be written")
    {
        auto dir = TempDir { "remove_reason" };
        dir.write("locked/victim.txt", "x");
        auto const locked = dir.path() / "locked";
        auto const writable = std::filesystem::status(locked).permissions();
        std::filesystem::permissions(
            locked,
            std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec,
            std::filesystem::perm_options::replace
        );
        auto probe_ec = std::error_code {};
        std::filesystem::create_directory(locked / "probe", probe_ec);
        if (!probe_ec) {
            std::filesystem::remove(locked / "probe", probe_ec);
            std::filesystem::permissions(locked, writable, std::filesystem::perm_options::replace);
            // Not SKIP: the suite is built -fno-exceptions, where Catch2 aborts the process instead.
            WARN("cannot revoke write permission (running as root?): scenario not exercised");
            return;
        }

        WHEN("the removal fails")
        {
            auto r = pup::platform::remove_file((locked / "victim.txt").string());
            std::filesystem::permissions(locked, writable, std::filesystem::perm_options::replace);

            THEN("the message carries the path and what the system said")
            {
                REQUIRE_FALSE(r);
                auto msg = std::string { r.error().msg() };
                INFO("msg: " << msg);
                REQUIRE(msg.find("victim.txt") != std::string::npos);
                REQUIRE(msg.find("Permission denied") != std::string::npos);
            }
        }
    }
}

SCENARIO("walk_directory does not descend symlinked directories on POSIX", "[platform][file_io][walk]")
{
    GIVEN("a directory containing a symlink to another directory")
    {
        auto dir = TempDir { "symlink" };
        dir.write("real/inside.txt", "x");
        std::filesystem::create_symlink("real", dir.path() / "link");
        auto root = dir.path().string();

        WHEN("the tree is walked")
        {
            auto seen = std::set<std::pair<std::string, bool>> {};
            (void)pup::platform::walk_directory(
                root,
                [&](pup::platform::DirEntry const& entry, std::string_view rel_path) -> bool {
                    seen.insert({ std::string { rel_path }, entry.is_dir });
                    return true;
                });

            THEN("the symlink is reported as a non-directory and not descended")
            {
                REQUIRE(seen.find({ "link", false }) != seen.end());
                REQUIRE(seen.find({ "link", true }) == seen.end());
                REQUIRE(seen.find({ "link/inside.txt", false }) == seen.end());
            }
        }
    }
}
#endif

SCENARIO("read_directory lists a directory's immediate entries", "[platform][file_io][walk]")
{
    GIVEN("a directory with files and subdirectories")
    {
        auto dir = TempDir { "readdir" };
        auto root = make_discovery_tree(dir);

        WHEN("the directory is read")
        {
            auto listing = pup::platform::DirEntries {};
            auto r = pup::platform::read_directory(root, listing);
            REQUIRE(r.has_value());

            auto seen = std::set<std::pair<std::string, bool>> {};
            for (auto const& e : listing.entries) {
                seen.insert({ std::string { e.name }, e.is_dir });
            }

            THEN("exactly the immediate children are returned")
            {
                auto expected = std::set<std::pair<std::string, bool>> {
                    { "a.txt", false },
                    { ".hidden", false },
                    { "sub", true },
                    { "skip", true },
                };
                REQUIRE(seen == expected);
            }
        }
    }
}

#ifndef _WIN32
namespace {

struct DeepWalkArgs {
    std::string root;
    bool completed = false;
    bool saw_leaf = false;
};

// Runs the walk on a thread whose stack is too small to survive a frame that
// embeds the directory listing (Buf's 4 KB inline buffer) once per level. An
// O(1) frame reaches the leaf; a per-frame listing overflows and crashes.
auto deep_walk_thread(void* p) -> void*
{
    auto* args = static_cast<DeepWalkArgs*>(p);
    (void)pup::platform::walk_directory(
        args->root,
        [&](pup::platform::DirEntry const& /*entry*/, std::string_view rel_path) -> bool {
            if (rel_path.size() >= 8 && rel_path.substr(rel_path.size() - 8) == "leaf.txt") {
                args->saw_leaf = true;
            }
            return true;
        });
    args->completed = true;
    return nullptr;
}

} // namespace

TEST_CASE("walk_directory descends deep trees without a per-frame stack blowup", "[platform][file_io][walk]")
{
    auto dir = TempDir { "deep" };
    constexpr auto depth = 200;
    auto rel = std::string {};
    for (auto i = 0; i < depth; ++i) {
        rel += "d/";
    }
    dir.write(rel + "leaf.txt", "x");
    auto root = dir.path().string();

    auto args = DeepWalkArgs { root, false, false };
    auto attr = pthread_attr_t {};
    REQUIRE(pthread_attr_init(&attr) == 0);
    REQUIRE(pthread_attr_setstacksize(&attr, std::size_t { 256 } * 1024) == 0);
    auto tid = pthread_t {};
    REQUIRE(pthread_create(&tid, &attr, deep_walk_thread, &args) == 0);
    pthread_join(tid, nullptr);
    pthread_attr_destroy(&attr);

    REQUIRE(args.completed);
    REQUIRE(args.saw_leaf);
}
#endif

SCENARIO("read_directory returns entries in name order", "[platform][file_io][walk]")
{
    GIVEN("a directory whose entries were created in reverse-lexicographic order")
    {
        // Mixed case on purpose: NTFS enumerates case-insensitively, so an
        // all-lowercase set would already arrive sorted and pin nothing there.
        auto const names = std::array<std::string_view, 4> {
            "Bravo", "Zulu", "alpha", "mike"
        };
        auto dir = TempDir { "order" };
        for (auto i = names.size(); i-- > 0;) {
            dir.write(names[i], "x");
        }

        WHEN("the directory is read")
        {
            auto listing = pup::platform::DirEntries {};
            REQUIRE(pup::platform::read_directory(dir.path().string(), listing).has_value());

            THEN("the entries are in name order")
            {
                auto seen = std::vector<std::string_view> {};
                for (auto const& entry : listing.entries) {
                    seen.push_back(entry.name);
                }
                REQUIRE(seen == std::vector<std::string_view>(names.begin(), names.end()));
            }
        }
    }
}

SCENARIO("walk_directory visits every level in name order", "[platform][file_io][walk]")
{
    GIVEN("a nested tree created in reverse-lexicographic order")
    {
        auto dir = TempDir { "walk_order" };
        for (auto name : std::array<std::string_view, 4> { "alpha.txt", "sub/bravo.txt", "sub/Yankee.txt", "Zulu.txt" }) {
            dir.write(name, "x");
        }

        WHEN("the tree is walked")
        {
            auto seen = std::vector<std::string> {};
            auto r = pup::platform::walk_directory(
                dir.path().string(),
                [&](pup::platform::DirEntry const& /*entry*/, std::string_view rel_path) -> bool {
                    seen.emplace_back(rel_path);
                    return true;
                });

            THEN("each directory is enumerated in name order, depth first")
            {
                REQUIRE(r.has_value());
                REQUIRE(seen == std::vector<std::string> { "Zulu.txt", "alpha.txt", "sub", "sub/Yankee.txt", "sub/bravo.txt" });
            }
        }
    }
}

namespace {

constexpr auto absent_a = std::string_view { "pup_no_such_a" };
constexpr auto absent_b = std::string_view { "pup_no_such_b" };

auto is_settled(std::string_view p) -> bool
{
    auto start = std::size_t { 0 };
    while (start <= p.size()) {
        auto end = p.find('/', start);
        if (end == std::string_view::npos) {
            end = p.size();
        }
        auto part = p.substr(start, end - start);
        if (part == "." || part == "..") {
            return false;
        }
        start = end + 1;
    }
    return true;
}

// Every path of up to four components over two names that do not exist and the two
// that mean something: the absent ones are what force the resolve-what-exists fallback.
auto unresolvable_suffixes() -> std::vector<std::string>
{
    auto const comps = std::array<std::string_view, 4> { absent_a, absent_b, "..", "." };
    auto out = std::vector<std::string> {};
    for (auto a : comps) {
        out.emplace_back(a);
        for (auto b : comps) {
            out.push_back(std::string { a } + "/" + std::string { b });
            for (auto c : comps) {
                out.push_back(std::string { a } + "/" + std::string { b } + "/" + std::string { c });
                for (auto d : comps) {
                    out.push_back(std::string { a } + "/" + std::string { b } + "/" + std::string { c } + "/" + std::string { d });
                }
            }
        }
    }
    return out;
}

} // namespace

SCENARIO("canonical settles a path that does not exist", "[platform][file_io][canonical]")
{
    GIVEN("paths built from names that are absent from the current directory")
    {
        auto cwd = pup::platform::current_directory();
        REQUIRE(cwd.has_value());
        auto const base = std::string { pup::global_pool().get(*cwd) };
        auto const base_prefix = base + "/";
        auto const absent_prefix = base_prefix + std::string { absent_a };
        auto const detour_prefix = absent_prefix + "/../";
        REQUIRE_FALSE(pup::platform::exists(absent_prefix));
        REQUIRE_FALSE(pup::platform::exists(base_prefix + std::string { absent_b }));

        WHEN("each is canonicalized")
        {
            THEN("the result is absolute and keeps no . or .. component")
            {
                for (auto const& suffix : unresolvable_suffixes()) {
                    auto const input = base_prefix + suffix;
                    auto const r = pup::platform::canonical(input);
                    REQUIRE(r.has_value());
                    auto const out = std::string { pup::global_pool().get(*r) };
                    INFO("in: " << input << "\nout: " << out);
                    REQUIRE(pup::path::is_absolute(out));
                    REQUIRE(is_settled(out));
                }
            }

            THEN("a detour that cancels itself names the same path as never taking it")
            {
                for (auto const& suffix : unresolvable_suffixes()) {
                    auto const detour = pup::platform::canonical(detour_prefix + suffix);
                    auto const direct = pup::platform::canonical(base_prefix + suffix);
                    REQUIRE(detour.has_value());
                    REQUIRE(direct.has_value());
                    INFO("suffix: " << suffix << "\ndetour: " << pup::global_pool().get(*detour) << "\ndirect: " << pup::global_pool().get(*direct));
                    REQUIRE(pup::global_pool().get(*detour) == pup::global_pool().get(*direct));
                }
            }
        }
    }
}

#ifndef _WIN32
SCENARIO("canonical resolves a symlinked prefix before cancelling what follows it", "[platform][file_io][canonical]")
{
    GIVEN("a symlink to a directory two levels down")
    {
        auto dir = TempDir { "canonical_symlink" };
        dir.write("deep/inner/keep.txt", "x");
        std::filesystem::create_symlink("deep/inner", dir.path() / "deeplink");
        auto const root = std::filesystem::canonical(dir.path()).string();

        WHEN("a path through the symlink climbs out with more .. than it has names")
        {
            auto const r = pup::platform::canonical(root + "/deeplink/nope/../../../z");

            THEN("the .. cancel against the resolved target, not against the link's own name")
            {
                REQUIRE(r.has_value());
                INFO("out: " << pup::global_pool().get(*r));
                REQUIRE(pup::global_pool().get(*r) == root + "/z");
            }
        }
    }
}
#endif
