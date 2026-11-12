// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include "expected.hpp"
#include "pup/core/source_location.hpp"
#include "pup/core/string_id.hpp"

#include <optional>
#include <string_view>

namespace pup {

/// Error categories for pup operations
enum class ErrorCode {
    // General errors
    None = 0,
    InvalidArgument,
    NotFound,
    IoError,
    InvalidState,

    // Index errors
    IndexVersionMismatch,
    IndexChecksumMismatch,
    // A code that conflates damage with an expected-benign outcome forces every caller to choose between noise and silence.
    IndexDamaged,

    // Parser errors
    ParseError,
    UnexpectedToken,
    IncludeNotFound,

    // Graph errors
    CyclicDependency,
    DuplicateNode,
    InvalidNodeId,
    UnknownMacro,
};

/// Error with code and message
struct Error {
    ErrorCode code = ErrorCode::None;
    StringId message = StringId::Empty;
    std::optional<SourcePosition> location = {};

    Error() = default;
    Error(ErrorCode c, StringId msg)
        : code(c)
        , message(msg)
    {
    }
    Error(ErrorCode c, std::string_view msg);

    /// Check if this represents no error
    [[nodiscard]]
    auto ok() const -> bool
    {
        return code == ErrorCode::None;
    }

    /// Check if this represents an error
    [[nodiscard]]
    explicit operator bool() const
    {
        return code != ErrorCode::None;
    }

    /// Get message as string_view (resolves StringId through global pool)
    [[nodiscard]]
    auto msg() const -> std::string_view;

    /// Create an error with a message
    [[nodiscard]]
    static auto make(ErrorCode c, StringId msg) -> Error
    {
        return Error { c, msg };
    }
};

/// How serious a printed message is
enum class Severity {
    Warning,
    Error,
};

class Buf;

/// Append a message as `file:line[:column]: severity: text`, or `severity: text` without a location:
/// the one form every message about a Tupfile is printed in.
auto append_message_line(
    Buf& out,
    std::optional<SourcePosition> const& location,
    Severity severity,
    std::string_view text
) -> void;

/// Result type alias for operations that may fail
template<typename T>
using Result = pup::expected<T, Error>;

/// Helper to create an error result from StringId
template<typename T>
[[nodiscard]]
auto make_error(ErrorCode code, StringId msg) -> Result<T>
{
    return pup::unexpected<Error>(Error::make(code, msg));
}

/// Non-template helper: interns message and returns Error (defined in result.cpp)
[[nodiscard]]
auto make_err_msg(ErrorCode code, std::string_view msg) -> Error;

/// Helper to create an error result from string_view (interns the message)
template<typename T, typename Msg>
requires std::is_convertible_v<Msg, std::string_view>
[[nodiscard]]
auto make_error(ErrorCode code, Msg&& msg) -> Result<T>
{
    return pup::unexpected<Error>(make_err_msg(code, std::string_view { std::forward<Msg>(msg) }));
}

} // namespace pup
