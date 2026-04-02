// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include "pup/core/result.hpp"

#include <cstddef>
#include <string_view>

namespace pup::platform {

struct AsyncProcess final {
    int pid = -1;
    int stdout_fd = -1;
    int stderr_fd = -1;

    [[nodiscard]]
    auto active() const -> bool { return pid > 0; }
};

struct SpawnOptions final {
    std::string_view command;
    std::string_view working_dir;
    char* const* env = nullptr; // nullptr = inherit
};

struct ProcessStatus final {
    bool exited = false;
    int exit_code = 0;
};

struct PollableFd final {
    int fd;
    bool is_stderr;
    std::size_t slot_index;
};

enum class Signal { Terminate, Kill };

[[nodiscard]]
auto spawn_async(SpawnOptions const& opts) -> Result<AsyncProcess>;

[[nodiscard]]
auto poll_fds(PollableFd* fds, std::size_t count, int timeout_ms) -> int;

[[nodiscard]]
auto read_nonblocking(int fd, char* buf, std::size_t size) -> int;

auto close_fd(int fd) -> void;

[[nodiscard]]
auto try_reap(int pid, ProcessStatus& out) -> bool;

auto reap(int pid, ProcessStatus& out) -> void;

auto send_signal(int pid, Signal sig) -> void;

} // namespace pup::platform
