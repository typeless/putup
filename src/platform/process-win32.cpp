// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/core/clock.hpp"
#include "pup/core/global_pool.hpp"
#include "pup/core/heap_buf.hpp"
#include "pup/core/string_pool.hpp"
#include "pup/platform/process.hpp"

#include <algorithm>
#include <cassert>
#include <string_view>
#include <windows.h>

namespace pup::platform {

auto build_env_strings(
    Vec<StringId> const& extra_env,
    bool inherit_env
) -> Vec<StringId>
{
    auto& pool = global_pool();
    auto result = Vec<StringId> {};

    if (inherit_env) {
        auto env_block = GetEnvironmentStringsW();
        if (env_block) {
            auto current = env_block;
            while (*current) {
                auto len = WideCharToMultiByte(CP_UTF8, 0, current, -1, nullptr, 0, nullptr, nullptr);
                if (len > 0) {
                    auto var = HeapBuf {};
                    var.resize(static_cast<std::size_t>(len - 1));
                    WideCharToMultiByte(CP_UTF8, 0, current, -1, var.data(), len, nullptr, nullptr);
                    result.push_back(pool.intern(var.view()));
                }
                current += wcslen(current) + 1;
            }
            FreeEnvironmentStringsW(env_block);
        }
    }

    for (auto var : extra_env) {
        result.push_back(var);
    }

    return result;
}

namespace {

auto create_env_block(Vec<StringId> const& env) -> std::wstring
{
    auto& pool = global_pool();
    auto block = std::wstring {};
    for (auto var : env) {
        auto sv = pool.get(var);
        auto len = MultiByteToWideChar(CP_UTF8, 0, sv.data(), -1, nullptr, 0);
        if (len > 0) {
            auto wvar = std::wstring(len - 1, L'\0');
            MultiByteToWideChar(CP_UTF8, 0, sv.data(), -1, wvar.data(), len);
            block += wvar;
            block += L'\0';
        }
    }
    block += L'\0';
    return block;
}

} // namespace

auto run_process(ProcessOptions const& opts) -> Result<ProcessResult>
{
    return run_process_with_callback(opts, nullptr, nullptr);
}

auto run_process_with_callback(
    ProcessOptions const& opts,
    ProcessOutputCallback callback,
    void* user_data
) -> Result<ProcessResult>
{
    auto start_time = pup::SteadyClock::now();

    // Create pipes for stdout/stderr
    auto sa = SECURITY_ATTRIBUTES { sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE };

    auto* stdout_read = static_cast<HANDLE>(nullptr);
    auto* stdout_write = static_cast<HANDLE>(nullptr);
    auto* stderr_read = static_cast<HANDLE>(nullptr);
    auto* stderr_write = static_cast<HANDLE>(nullptr);
    auto* stdin_read = static_cast<HANDLE>(nullptr);
    auto* stdin_write = static_cast<HANDLE>(nullptr);

    if (opts.capture_stdout) {
        if (!CreatePipe(&stdout_read, &stdout_write, &sa, 0)) {
            return make_error<ProcessResult>(ErrorCode::IoError, "Failed to create stdout pipe");
        }
        SetHandleInformation(stdout_read, HANDLE_FLAG_INHERIT, 0);
    }

    if (opts.capture_stderr) {
        if (!CreatePipe(&stderr_read, &stderr_write, &sa, 0)) {
            if (stdout_read) {
                CloseHandle(stdout_read);
            }
            if (stdout_write) {
                CloseHandle(stdout_write);
            }
            return make_error<ProcessResult>(ErrorCode::IoError, "Failed to create stderr pipe");
        }
        SetHandleInformation(stderr_read, HANDLE_FLAG_INHERIT, 0);
    }

    if (opts.stdin_data) {
        if (!CreatePipe(&stdin_read, &stdin_write, &sa, 0)) {
            if (stdout_read) {
                CloseHandle(stdout_read);
            }
            if (stdout_write) {
                CloseHandle(stdout_write);
            }
            if (stderr_read) {
                CloseHandle(stderr_read);
            }
            if (stderr_write) {
                CloseHandle(stderr_write);
            }
            return make_error<ProcessResult>(ErrorCode::IoError, "Failed to create stdin pipe");
        }
        SetHandleInformation(stdin_write, HANDLE_FLAG_INHERIT, 0);
    }

    // Build command line: cmd.exe /c "command"
    auto& pool = global_pool();
    auto cmd_sv = pool.get(opts.command);
    auto cmdline = std::wstring { L"cmd.exe /c \"" };
    auto cmd_len = MultiByteToWideChar(CP_UTF8, 0, cmd_sv.data(), static_cast<int>(cmd_sv.size()), nullptr, 0);
    if (cmd_len > 0) {
        auto wcmd = std::wstring(cmd_len, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, cmd_sv.data(), static_cast<int>(cmd_sv.size()), wcmd.data(), cmd_len);
        cmdline += wcmd;
    }
    cmdline += L'"';

    // Setup process startup info
    auto si = STARTUPINFOW {};
    si.cb = sizeof(STARTUPINFOW);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = stdin_read ? stdin_read : GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = stdout_write ? stdout_write : GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError = stderr_write ? stderr_write : GetStdHandle(STD_ERROR_HANDLE);

    auto pi = PROCESS_INFORMATION {};

    // Build environment block
    auto env_strings = build_env_strings(opts.env, opts.inherit_env);
    auto env_block = create_env_block(env_strings);

    // Convert working directory
    auto working_dir = std::wstring {};
    auto wd_sv = pool.get(opts.working_dir);
    if (!wd_sv.empty()) {
        auto len = MultiByteToWideChar(CP_UTF8, 0, wd_sv.data(), static_cast<int>(wd_sv.size()), nullptr, 0);
        if (len > 0) {
            working_dir.resize(len);
            MultiByteToWideChar(CP_UTF8, 0, wd_sv.data(), static_cast<int>(wd_sv.size()), working_dir.data(), len);
        }
    }

    // Create process
    auto created = CreateProcessW(
        nullptr,
        cmdline.data(),
        nullptr,
        nullptr,
        TRUE,
        CREATE_UNICODE_ENVIRONMENT,
        env_block.data(),
        working_dir.empty() ? nullptr : working_dir.c_str(),
        &si,
        &pi
    );

    // Close write ends in parent
    if (stdout_write) {
        CloseHandle(stdout_write);
    }
    if (stderr_write) {
        CloseHandle(stderr_write);
    }
    if (stdin_read) {
        CloseHandle(stdin_read);
    }

    if (!created) {
        if (stdout_read) {
            CloseHandle(stdout_read);
        }
        if (stderr_read) {
            CloseHandle(stderr_read);
        }
        if (stdin_write) {
            CloseHandle(stdin_write);
        }
        return make_error<ProcessResult>(ErrorCode::IoError, "Failed to create process");
    }

    // Write stdin data
    if (opts.stdin_data && stdin_write) {
        auto written = DWORD {};
        auto stdin_sv = pool.get(*opts.stdin_data);
        WriteFile(stdin_write, stdin_sv.data(), static_cast<DWORD>(stdin_sv.size()), &written, nullptr);
        CloseHandle(stdin_write);
        stdin_write = nullptr;
    }

    auto result = ProcessResult {};
    auto timed_out = false;
    auto stdout_buf = HeapBuf {};
    auto stderr_buf = HeapBuf {};

    // Read stdout/stderr
    auto deadline = opts.timeout
        ? std::optional { pup::SteadyClock::now() + *opts.timeout }
        : std::nullopt;

    char buffer[4096];
    auto bytes_read = DWORD {};
    auto stdout_open = stdout_read != nullptr;
    auto stderr_open = stderr_read != nullptr;

    while (stdout_open || stderr_open) {
        if (deadline) {
            auto remaining = *deadline - pup::SteadyClock::now();
            if (remaining <= std::chrono::milliseconds::zero()) {
                timed_out = true;
                break;
            }
        }

        if (stdout_open) {
            auto available = DWORD { 0 };
            if (PeekNamedPipe(stdout_read, nullptr, 0, nullptr, &available, nullptr) && available > 0) {
                if (ReadFile(stdout_read, buffer, sizeof(buffer), &bytes_read, nullptr) && bytes_read > 0) {
                    auto data = std::string_view { buffer, bytes_read };
                    if (callback) {
                        callback(data, false, user_data);
                    }
                    stdout_buf.append(data);
                }
            } else {
                // Check if process has exited
                auto exit_code = DWORD {};
                if (GetExitCodeProcess(pi.hProcess, &exit_code) && exit_code != STILL_ACTIVE) {
                    // Read any remaining data
                    while (ReadFile(stdout_read, buffer, sizeof(buffer), &bytes_read, nullptr) && bytes_read > 0) {
                        auto data = std::string_view { buffer, bytes_read };
                        if (callback) {
                            callback(data, false, user_data);
                        }
                        stdout_buf.append(data);
                    }
                    stdout_open = false;
                }
            }
        }

        if (stderr_open) {
            auto available = DWORD { 0 };
            if (PeekNamedPipe(stderr_read, nullptr, 0, nullptr, &available, nullptr) && available > 0) {
                if (ReadFile(stderr_read, buffer, sizeof(buffer), &bytes_read, nullptr) && bytes_read > 0) {
                    auto data = std::string_view { buffer, bytes_read };
                    if (callback) {
                        callback(data, true, user_data);
                    }
                    stderr_buf.append(data);
                }
            } else {
                auto exit_code = DWORD {};
                if (GetExitCodeProcess(pi.hProcess, &exit_code) && exit_code != STILL_ACTIVE) {
                    while (ReadFile(stderr_read, buffer, sizeof(buffer), &bytes_read, nullptr) && bytes_read > 0) {
                        auto data = std::string_view { buffer, bytes_read };
                        if (callback) {
                            callback(data, true, user_data);
                        }
                        stderr_buf.append(data);
                    }
                    stderr_open = false;
                }
            }
        }

        Sleep(1); // Avoid busy wait
    }

    if (stdout_read) {
        CloseHandle(stdout_read);
    }
    if (stderr_read) {
        CloseHandle(stderr_read);
    }

    // Handle timeout
    if (timed_out) {
        TerminateProcess(pi.hProcess, 1);
        result.timed_out = true;
    }

    // Wait for process to finish
    WaitForSingleObject(pi.hProcess, INFINITE);

    auto exit_code = DWORD {};
    if (GetExitCodeProcess(pi.hProcess, &exit_code)) {
        result.exit_code = static_cast<int>(exit_code);
    }

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    auto end_time = pup::SteadyClock::now();
    result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);

    result.stdout_output = pool.intern(stdout_buf.view());
    result.stderr_output = pool.intern(stderr_buf.view());

    return result;
}

auto run_parallel_tasks(
    int (*task)(void* ctx),
    void** contexts,
    std::size_t count
) -> int
{
    // Windows: sequential fallback (no fork)
    auto failed = 0;
    for (std::size_t i = 0; i < count; ++i) {
        if (task(contexts[i]) != 0) {
            ++failed;
        }
    }
    return failed;
}

auto spawn_async(SpawnOptions const& opts) -> Result<AsyncProcess>
{
    auto sa = SECURITY_ATTRIBUTES { sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE };

    auto* stdout_read = static_cast<HANDLE>(nullptr);
    auto* stdout_write = static_cast<HANDLE>(nullptr);
    auto* stderr_read = static_cast<HANDLE>(nullptr);
    auto* stderr_write = static_cast<HANDLE>(nullptr);

    if (!CreatePipe(&stdout_read, &stdout_write, &sa, 0)) {
        return make_error<AsyncProcess>(ErrorCode::IoError, "Failed to create stdout pipe");
    }
    SetHandleInformation(stdout_read, HANDLE_FLAG_INHERIT, 0);

    if (!CreatePipe(&stderr_read, &stderr_write, &sa, 0)) {
        CloseHandle(stdout_read);
        CloseHandle(stdout_write);
        return make_error<AsyncProcess>(ErrorCode::IoError, "Failed to create stderr pipe");
    }
    SetHandleInformation(stderr_read, HANDLE_FLAG_INHERIT, 0);

    // Build command line: cmd.exe /c "command"
    auto cmdline = std::wstring { L"cmd.exe /c \"" };
    auto cmd_len = MultiByteToWideChar(CP_UTF8, 0, opts.command.data(), static_cast<int>(opts.command.size()), nullptr, 0);
    if (cmd_len > 0) {
        auto wcmd = std::wstring(cmd_len, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, opts.command.data(), static_cast<int>(opts.command.size()), wcmd.data(), cmd_len);
        cmdline += wcmd;
    }
    cmdline += L'"';

    auto si = STARTUPINFOW {};
    si.cb = sizeof(STARTUPINFOW);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = stdout_write;
    si.hStdError = stderr_write;

    // Build environment block (double-null-terminated wide string)
    auto env_block = std::wstring {};
    auto has_env = opts.env != nullptr;
    if (has_env) {
        for (auto* p = opts.env; *p != nullptr; ++p) {
            auto sv = std::string_view { *p };
            auto len = MultiByteToWideChar(CP_UTF8, 0, sv.data(), static_cast<int>(sv.size()), nullptr, 0);
            if (len > 0) {
                auto wvar = std::wstring(len, L'\0');
                MultiByteToWideChar(CP_UTF8, 0, sv.data(), static_cast<int>(sv.size()), wvar.data(), len);
                env_block += wvar;
                env_block += L'\0';
            }
        }
        env_block += L'\0';
    }

    // Convert working directory
    auto working_dir = std::wstring {};
    if (!opts.working_dir.empty()) {
        auto len = MultiByteToWideChar(CP_UTF8, 0, opts.working_dir.data(), static_cast<int>(opts.working_dir.size()), nullptr, 0);
        if (len > 0) {
            working_dir.resize(len);
            MultiByteToWideChar(CP_UTF8, 0, opts.working_dir.data(), static_cast<int>(opts.working_dir.size()), working_dir.data(), len);
        }
    }

    auto pi = PROCESS_INFORMATION {};
    auto created = CreateProcessW(
        nullptr,
        cmdline.data(),
        nullptr,
        nullptr,
        TRUE,
        CREATE_UNICODE_ENVIRONMENT,
        has_env ? env_block.data() : nullptr,
        working_dir.empty() ? nullptr : working_dir.c_str(),
        &si,
        &pi
    );

    CloseHandle(stdout_write);
    CloseHandle(stderr_write);

    if (!created) {
        CloseHandle(stdout_read);
        CloseHandle(stderr_read);
        return make_error<AsyncProcess>(ErrorCode::IoError, "Failed to create process");
    }

    CloseHandle(pi.hThread);

    return AsyncProcess {
        .pid = reinterpret_cast<std::intptr_t>(pi.hProcess),    // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
        .stdout_fd = reinterpret_cast<std::intptr_t>(stdout_read),  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
        .stderr_fd = reinterpret_cast<std::intptr_t>(stderr_read),  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
    };
}

auto poll_fds(PollableFd* fds, std::size_t count, int timeout_ms) -> int
{
    if (count == 0) {
        Sleep(static_cast<DWORD>(std::max(std::min(timeout_ms, 100), 0)));
        return 0;
    }

    // Save original fds for restoration on ready
    std::intptr_t stack_originals[64]; // NOLINT(modernize-avoid-c-arrays)
    auto* originals = count <= 64 ? stack_originals : new std::intptr_t[count]; // NOLINT
    for (std::size_t i = 0; i < count; ++i) {
        originals[i] = fds[i].fd;
    }

    auto const infinite = timeout_ms < 0;
    auto const deadline = infinite ? ULONGLONG { 0 } : GetTickCount64() + static_cast<ULONGLONG>(timeout_ms);

    for (;;) {
        auto found = 0;

        for (std::size_t i = 0; i < count; ++i) {
            if (originals[i] == -1) {
                continue;
            }

            auto h = reinterpret_cast<HANDLE>(originals[i]); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
            auto available = DWORD { 0 };
            auto ok = PeekNamedPipe(h, nullptr, 0, nullptr, &available, nullptr);

            if (!ok || available > 0) {
                fds[i].fd = originals[i];
                ++found;
            } else {
                fds[i].fd = -1;
            }
        }

        if (found > 0) {
            if (count > 64) { delete[] originals; } // NOLINT
            return found;
        }

        if (!infinite && GetTickCount64() >= deadline) {
            if (count > 64) { delete[] originals; } // NOLINT
            return 0;
        }

        Sleep(1);
    }
}

auto read_nonblocking(std::intptr_t fd, char* buf, std::size_t size) -> int
{
    auto h = reinterpret_cast<HANDLE>(fd); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
    auto available = DWORD { 0 };
    if (!PeekNamedPipe(h, nullptr, 0, nullptr, &available, nullptr)) {
        return 0; // Broken pipe = EOF
    }
    if (available == 0) {
        return -1; // Would block
    }
    auto to_read = static_cast<DWORD>(std::min(static_cast<std::size_t>(available), size));
    auto bytes_read = DWORD { 0 };
    if (!ReadFile(h, buf, to_read, &bytes_read, nullptr)) {
        return 0; // Read error = treat as EOF
    }
    return static_cast<int>(bytes_read);
}

auto close_fd(std::intptr_t fd) -> void
{
    if (fd != -1) {
        CloseHandle(reinterpret_cast<HANDLE>(fd)); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
    }
}

auto try_reap(std::intptr_t pid, ProcessStatus& out) -> bool
{
    auto h = reinterpret_cast<HANDLE>(pid); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
    // Use WaitForSingleObject(0) instead of just GetExitCodeProcess to avoid
    // false positive when process exits with code 259 (STILL_ACTIVE)
    if (WaitForSingleObject(h, 0) != WAIT_OBJECT_0) {
        return false;
    }
    auto code = DWORD { 0 };
    GetExitCodeProcess(h, &code);
    out.exited = true;
    out.exit_code = static_cast<int>(code);
    CloseHandle(h); // match POSIX semantic: successful try_reap consumes the process
    return true;
}

auto reap(std::intptr_t pid, ProcessStatus& out) -> void
{
    auto h = reinterpret_cast<HANDLE>(pid); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
    WaitForSingleObject(h, INFINITE);
    auto code = DWORD { 0 };
    GetExitCodeProcess(h, &code);
    out.exited = true;
    out.exit_code = static_cast<int>(code);
    CloseHandle(h);
}

auto send_signal(std::intptr_t pid, Signal /*sig*/) -> void
{
    // Windows has no graceful termination signal (SIGTERM equivalent) for
    // console processes. Both Terminate and Kill map to TerminateProcess.
    assert(pid != -1);
    auto h = reinterpret_cast<HANDLE>(pid); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
    TerminateProcess(h, 1);
}

} // namespace pup::platform
