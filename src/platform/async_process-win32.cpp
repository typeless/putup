// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/platform/async_process.hpp"

#include <cassert>

namespace pup::platform {

auto spawn_async(SpawnOptions const& /*opts*/) -> Result<AsyncProcess>
{
    return make_error<AsyncProcess>(ErrorCode::IoError, "Async process spawning not supported on Windows");
}

auto poll_fds(PollableFd* /*fds*/, std::size_t /*count*/, int /*timeout_ms*/) -> int
{
    assert(false && "poll_fds not supported on Windows");
    return -1;
}

auto read_nonblocking(int /*fd*/, char* /*buf*/, std::size_t /*size*/) -> int
{
    assert(false && "read_nonblocking not supported on Windows");
    return -1;
}

auto close_fd(int /*fd*/) -> void
{
    assert(false && "close_fd not supported on Windows");
}

auto try_reap(int /*pid*/, ProcessStatus& /*out*/) -> bool
{
    assert(false && "try_reap not supported on Windows");
    return false;
}

auto reap(int /*pid*/, ProcessStatus& /*out*/) -> void
{
    assert(false && "reap not supported on Windows");
}

auto send_signal(int /*pid*/, Signal /*sig*/) -> void
{
    assert(false && "send_signal not supported on Windows");
}

} // namespace pup::platform
