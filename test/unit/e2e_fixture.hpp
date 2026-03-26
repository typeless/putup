// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include "pup/exec/runner.hpp"
#include "pup/platform/env.hpp"

#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pup::test {

/// RAII guard for environment variables
class EnvGuard {
public:
    explicit EnvGuard(std::string name, std::string value)
        : m_name { std::move(name) }
    {
        if (auto const* existing = std::getenv(m_name.c_str()))
            m_previous = existing;
        pup::platform::set_env(m_name, value);
    }

    ~EnvGuard()
    {
        if (m_previous)
            pup::platform::set_env(m_name, *m_previous);
        else
            pup::platform::unset_env(m_name);
    }

    EnvGuard(EnvGuard const&) = delete;
    auto operator=(EnvGuard const&) -> EnvGuard& = delete;
    EnvGuard(EnvGuard&&) = delete;
    auto operator=(EnvGuard&&) -> EnvGuard& = delete;

private:
    std::string m_name;
    std::optional<std::string> m_previous;
};

/// Result of running pup command
struct PupResult {
    int exit_code = 0;
    std::string stdout_output;
    std::string stderr_output;

    [[nodiscard]] auto success() const -> bool { return exit_code == 0; }
    [[nodiscard]] auto is_noop() const -> bool
    {
        return stdout_output.find("Nothing to do") != std::string::npos
            || stdout_output.find("0 commands") != std::string::npos;
    }
};

/// Result of running an executable
struct ProcessResult {
    int exit_code = 0;
    std::string stdout_output;
    std::string stderr_output;

    [[nodiscard]] auto success() const -> bool { return exit_code == 0; }
};

/// E2E test fixture for testing pup builds
///
/// Copies fixture files to a temp directory, provides methods for
/// running pup commands and verifying outputs.
///
/// Example:
/// ```cpp
/// SCENARIO("Building a simple C project", "[e2e][build]") {
///     GIVEN("an initialized simple_c project") {
///         auto f = E2EFixture{"simple_c"};
///         REQUIRE(f.init().success());
///
///         WHEN("the project is built") {
///             auto result = f.build();
///             THEN("executable is created") {
///                 REQUIRE(f.is_executable("hello"));
///             }
///         }
///     }
/// }
/// ```
class E2EFixture {
public:
    /// Create fixture from test/e2e/fixtures/<name>
    explicit E2EFixture(std::string_view name);

    /// Cleanup temp directory (unless KEEP_WORKDIR env var is set)
    ~E2EFixture();

    // Non-copyable, movable
    E2EFixture(E2EFixture const&) = delete;
    auto operator=(E2EFixture const&) -> E2EFixture& = delete;
    E2EFixture(E2EFixture&&) noexcept;
    auto operator=(E2EFixture&&) noexcept -> E2EFixture&;

    // Build operations
    [[nodiscard]] auto init() -> PupResult;
    [[nodiscard]] auto build(std::vector<std::string> const& args = {}) -> PupResult;
    [[nodiscard]] auto clean(std::vector<std::string> const& args = {}) -> PupResult;
    [[nodiscard]] auto distclean(std::vector<std::string> const& args = {}) -> PupResult;
    [[nodiscard]] auto parse(std::vector<std::string> const& args = {}) -> PupResult;
    [[nodiscard]] auto pup(std::vector<std::string> const& args) -> PupResult;

    // Verification
    [[nodiscard]] auto exists(std::string_view path) const -> bool;
    [[nodiscard]] auto is_file(std::string_view path) const -> bool;
    [[nodiscard]] auto is_directory(std::string_view path) const -> bool;
    [[nodiscard]] auto is_executable(std::string_view path) const -> bool;
    [[nodiscard]] auto read_file(std::string_view path) const -> pup::String;

    // Execute programs
    [[nodiscard]] auto run(
        std::string_view path, std::vector<std::string> const& args = {}
    ) -> ProcessResult;

    // Modification
    auto write_file(std::string_view path, std::string_view content) -> void;
    auto append_file(std::string_view path, std::string_view content) -> void;
    auto remove_file(std::string_view path) -> void;
    auto mkdir(std::string_view path) -> void;
    auto create_symlink(std::string_view target, std::string_view link) -> void;

    /// Run pup from a specific directory within the workdir
    [[nodiscard]] auto run_pup_in_dir(
        std::string_view dir, std::vector<std::string> const& args = {}
    ) -> PupResult;

    // Accessors
    [[nodiscard]] auto workdir() const -> std::filesystem::path const& { return m_workdir; }
    [[nodiscard]] auto name() const -> std::string const& { return m_name; }

private:
    std::string m_name;
    std::filesystem::path m_workdir;
    std::filesystem::path m_fixture_dir;
    std::filesystem::path m_pup_binary;
    exec::CommandRunner m_runner;

    auto run_pup(std::vector<std::string> const& args) -> PupResult;
    auto resolve_path(std::string_view path) const -> std::filesystem::path;
};

/// Run a shell fixture script (test.sh)
/// Used for tests that need to stay as shell scripts
[[nodiscard]] auto run_shell_fixture(std::string_view name) -> ProcessResult;

/// Get path to pup binary (auto-detected or from environment)
[[nodiscard]] auto get_pup_binary() -> std::filesystem::path;

/// Get path to fixtures directory
[[nodiscard]] auto get_fixtures_dir() -> std::filesystem::path;

} // namespace pup::test
