// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/core/result.hpp"
#include "pup/core/global_pool.hpp"
#include "pup/core/string_pool.hpp"

namespace pup {

Error::Error(ErrorCode c, std::string_view msg)
    : code(c)
    , message(global_pool().intern(msg))
{
}

auto Error::msg() const -> std::string_view
{
    return global_pool().get(message);
}

auto make_err_msg(ErrorCode code, std::string_view msg) -> Error
{
    return Error::make(code, global_pool().intern(msg));
}

} // namespace pup
