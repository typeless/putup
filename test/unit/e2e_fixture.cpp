// SPDX-License-Identifier: MIT
// Copyright (c) 2024 pup authors

#include "e2e_fixture.hpp"

#include <cstdlib>
#include <fstream>
#include <random>
#include <sstream>

namespace pup::test {

namespace fs = std::filesystem;

namespace {

auto generate_temp_dir() -> fs::path
{
    auto const* tmpdir = std::getenv("TMPDIR");
    if (!tmpdir)
        tmpdir = "/tmp/claude";

    auto base = fs::path { tmpdir };
    if (!fs::exists(base))
        fs::create_directories(base);

    // Generate random suffix
    auto rng = std::random_device {};
    auto dist = std::uniform_int_distribution<unsigned int> { 0, 0xFFFFFFFF };
    auto suffix = std::to_string(dist(rng));

    auto result = base / ("e2e_" + suffix);
    fs::create_directories(result);
    return result;
}

auto copy_fixture(fs::path const& src, fs::path const& dst) -> void
{
    for (auto const& entry : fs::recursive_directory_iterator(src)) {
        auto relative = fs::relative(entry.path(), src);
        auto target = dst / relative;

        if (entry.is_directory()) {
            fs::create_directories(target);
        } else if (entry.is_regular_file()) {
            fs::create_directories(target.parent_path());

            // Rename Tupfile.fixture to Tupfile
            if (entry.path().filename() == "Tupfile.fixture") {
                fs::copy_file(entry.path(), target.parent_path() / "Tupfile");
            } else if (entry.path().filename() != "test.sh") {
                // Skip test.sh - we don't need it for C++ tests
                fs::copy_file(entry.path(), target);
            }
        } else if (entry.is_symlink()) {
            auto link_target = fs::read_symlink(entry.path());
            fs::create_symlink(link_target, target);
        }
    }
}

} // namespace

E2EFixture::E2EFixture(std::string_view name)
    : m_name { name }
    , m_workdir { generate_temp_dir() }
    , m_fixture_dir { get_fixtures_dir() / name }
    , m_pup_binary { get_pup_binary() }
{
    if (!fs::exists(m_fixture_dir)) {
        throw std::runtime_error("Fixture not found: " + std::string { name });
    }

    copy_fixture(m_fixture_dir, m_workdir);
    m_runner.set_working_dir(m_workdir);
}

E2EFixture::~E2EFixture()
{
    auto const* keep = std::getenv("KEEP_WORKDIR");
    if (keep && std::string { keep } == "1") {
        // Don't delete - useful for debugging
        return;
    }

    if (!m_workdir.empty() && fs::exists(m_workdir)) {
        auto ec = std::error_code {};
        fs::remove_all(m_workdir, ec);
    }
}

E2EFixture::E2EFixture(E2EFixture&& other) noexcept
    : m_name { std::move(other.m_name) }
    , m_workdir { std::move(other.m_workdir) }
    , m_fixture_dir { std::move(other.m_fixture_dir) }
    , m_pup_binary { std::move(other.m_pup_binary) }
    , m_runner { std::move(other.m_runner) }
{
    other.m_workdir.clear(); // Prevent cleanup in moved-from object
}

auto E2EFixture::operator=(E2EFixture&& other) noexcept -> E2EFixture&
{
    if (this != &other) {
        // Clean up current workdir
        if (!m_workdir.empty() && fs::exists(m_workdir)) {
            auto ec = std::error_code {};
            fs::remove_all(m_workdir, ec);
        }

        m_name = std::move(other.m_name);
        m_workdir = std::move(other.m_workdir);
        m_fixture_dir = std::move(other.m_fixture_dir);
        m_pup_binary = std::move(other.m_pup_binary);
        m_runner = std::move(other.m_runner);

        other.m_workdir.clear();
    }
    return *this;
}

auto E2EFixture::run_pup(std::vector<std::string> const& args) -> PupResult
{
    auto cmd = std::ostringstream {};
    cmd << m_pup_binary.string();
    for (auto const& arg : args) {
        cmd << " " << exec::shell_quote(arg);
    }

    auto result = m_runner.run(cmd.str());
    if (!result) {
        return PupResult { .exit_code = -1, .stdout_output = {}, .stderr_output = "Failed to run pup" };
    }

    return PupResult {
        .exit_code = result->exit_code,
        .stdout_output = result->stdout_output,
        .stderr_output = result->stderr_output,
    };
}

auto E2EFixture::init() -> PupResult
{
    return run_pup({ "init" });
}

auto E2EFixture::build(std::vector<std::string> const& args) -> PupResult
{
    return run_pup(args);
}

auto E2EFixture::clean(std::vector<std::string> const& args) -> PupResult
{
    auto full_args = std::vector<std::string> { "clean" };
    full_args.insert(full_args.end(), args.begin(), args.end());
    return run_pup(full_args);
}

auto E2EFixture::distclean(std::vector<std::string> const& args) -> PupResult
{
    auto full_args = std::vector<std::string> { "distclean" };
    full_args.insert(full_args.end(), args.begin(), args.end());
    return run_pup(full_args);
}

auto E2EFixture::parse(std::vector<std::string> const& args) -> PupResult
{
    auto full_args = std::vector<std::string> { "parse" };
    full_args.insert(full_args.end(), args.begin(), args.end());
    return run_pup(full_args);
}

auto E2EFixture::pup(std::vector<std::string> const& args) -> PupResult
{
    return run_pup(args);
}

auto E2EFixture::resolve_path(std::string_view path) const -> fs::path
{
    auto p = fs::path { path };
    if (p.is_absolute())
        return p;
    return m_workdir / p;
}

auto E2EFixture::exists(std::string_view path) const -> bool
{
    return fs::exists(resolve_path(path));
}

auto E2EFixture::is_file(std::string_view path) const -> bool
{
    return fs::is_regular_file(resolve_path(path));
}

auto E2EFixture::is_directory(std::string_view path) const -> bool
{
    return fs::is_directory(resolve_path(path));
}

auto E2EFixture::is_executable(std::string_view path) const -> bool
{
    auto p = resolve_path(path);
    if (!fs::exists(p))
        return false;

    auto status = fs::status(p);
    auto perms = status.permissions();
    return (perms & fs::perms::owner_exec) != fs::perms::none;
}

auto E2EFixture::read_file(std::string_view path) const -> std::string
{
    auto p = resolve_path(path);
    auto ifs = std::ifstream { p };
    if (!ifs)
        return {};

    auto ss = std::ostringstream {};
    ss << ifs.rdbuf();
    return ss.str();
}

auto E2EFixture::run(std::string_view path, std::vector<std::string> const& args) -> ProcessResult
{
    auto cmd = std::ostringstream {};
    auto p = resolve_path(path);
    cmd << p.string();
    for (auto const& arg : args) {
        cmd << " " << exec::shell_quote(arg);
    }

    auto result = m_runner.run(cmd.str());
    if (!result) {
        return ProcessResult { .exit_code = -1, .stdout_output = {}, .stderr_output = "Failed to run process" };
    }

    return ProcessResult {
        .exit_code = result->exit_code,
        .stdout_output = result->stdout_output,
        .stderr_output = result->stderr_output,
    };
}

auto E2EFixture::write_file(std::string_view path, std::string_view content) -> void
{
    auto p = resolve_path(path);
    fs::create_directories(p.parent_path());

    auto ofs = std::ofstream { p };
    ofs << content;
}

auto E2EFixture::append_file(std::string_view path, std::string_view content) -> void
{
    auto p = resolve_path(path);
    auto ofs = std::ofstream { p, std::ios::app };
    ofs << content;
}

auto E2EFixture::remove_file(std::string_view path) -> void
{
    auto p = resolve_path(path);
    auto ec = std::error_code {};
    fs::remove(p, ec);
}

auto E2EFixture::mkdir(std::string_view path) -> void
{
    auto p = resolve_path(path);
    fs::create_directories(p);
}

auto E2EFixture::create_symlink(std::string_view target, std::string_view link) -> void
{
    auto link_path = resolve_path(link);
    fs::create_directories(link_path.parent_path());
    fs::create_symlink(target, link_path);
}

auto E2EFixture::run_pup_in_dir(
    std::string_view dir, std::vector<std::string> const& args
) -> PupResult
{
    auto working_dir = resolve_path(dir);

    auto cmd = std::ostringstream {};
    cmd << m_pup_binary.string();
    for (auto const& arg : args) {
        cmd << " " << exec::shell_quote(arg);
    }

    auto opts = exec::RunOptions {
        .working_dir = working_dir,
        .inherit_env = true,
    };

    auto result = m_runner.run(cmd.str(), opts);
    if (!result) {
        return PupResult { .exit_code = -1, .stdout_output = {}, .stderr_output = "Failed to run pup" };
    }

    return PupResult {
        .exit_code = result->exit_code,
        .stdout_output = result->stdout_output,
        .stderr_output = result->stderr_output,
    };
}

auto run_shell_fixture(std::string_view name) -> ProcessResult
{
    auto fixtures_dir = get_fixtures_dir();
    auto fixture_dir = fixtures_dir / name;
    auto test_script = fixture_dir / "test.sh";

    if (!fs::exists(test_script)) {
        return ProcessResult { .exit_code = -1, .stdout_output = {}, .stderr_output = "test.sh not found" };
    }

    // Create temp directory
    auto workdir = generate_temp_dir();
    copy_fixture(fixture_dir, workdir);

    // Copy test.sh too for shell fixtures
    fs::copy_file(test_script, workdir / "test.sh");
    fs::permissions(workdir / "test.sh", fs::perms::owner_exec | fs::perms::owner_read);

    auto runner = exec::CommandRunner {};
    auto opts = exec::RunOptions {
        .working_dir = workdir,
        .env = { "PUP=" + get_pup_binary().string() },
        .inherit_env = true,
    };

    auto result = runner.run("./test.sh", opts);

    // Cleanup unless KEEP_WORKDIR
    auto const* keep = std::getenv("KEEP_WORKDIR");
    if (!keep || std::string { keep } != "1") {
        auto ec = std::error_code {};
        fs::remove_all(workdir, ec);
    }

    if (!result) {
        return ProcessResult { .exit_code = -1, .stdout_output = {}, .stderr_output = "Failed to run test.sh" };
    }

    return ProcessResult {
        .exit_code = result->exit_code,
        .stdout_output = result->stdout_output,
        .stderr_output = result->stderr_output,
    };
}

auto get_pup_binary() -> fs::path
{
    // Check environment variable first
    if (auto const* env = std::getenv("PUP")) {
        return fs::path { env };
    }

    // Try to find relative to current executable or common paths
    auto candidates = std::vector<fs::path> {
        "./build/pup",
        "../build/pup",
        "../../build/pup",
        "../../../build/pup",
    };

    for (auto const& candidate : candidates) {
        if (fs::exists(candidate))
            return fs::canonical(candidate);
    }

    throw std::runtime_error("pup binary not found. Set PUP environment variable.");
}

auto get_fixtures_dir() -> fs::path
{
    // Check environment variable first
    if (auto const* env = std::getenv("E2E_FIXTURES")) {
        return fs::path { env };
    }

    // Try common relative paths
    auto candidates = std::vector<fs::path> {
        "./test/e2e/fixtures",
        "../test/e2e/fixtures",
        "../../test/e2e/fixtures",
        "../../../test/e2e/fixtures",
    };

    for (auto const& candidate : candidates) {
        if (fs::exists(candidate))
            return fs::canonical(candidate);
    }

    throw std::runtime_error("E2E fixtures not found. Set E2E_FIXTURES environment variable.");
}

} // namespace pup::test
