// SPDX-License-Identifier: MIT
// Copyright (c) 2024 pup authors

#pragma once

#include "expected.hpp"

#include <string>
#include <string_view>

namespace pup {

/// Error categories for pup operations
enum class ErrorCode {
    // General errors
    None = 0,
    Unknown,
    InvalidArgument,
    NotFound,
    AlreadyExists,
    PermissionDenied,
    IoError,

    // Index errors
    IndexCorrupted,
    IndexVersionMismatch,
    IndexChecksumMismatch,
    IndexTruncated,

    // Parser errors
    ParseError,
    LexerError,
    UnexpectedToken,
    UnterminatedString,
    CircularInclude,
    IncludeNotFound,
    InvalidVariableRef,
    InvalidPattern,

    // Graph errors
    CyclicDependency,
    DuplicateNode,
    InvalidNodeId,
    InvalidEdge,
    UnknownMacro,

    // Index errors
    InvalidFormat,
    InvalidState,

    // Execution errors
    CommandFailed,
    MissingInput,
    OutputMismatch,
};

/// Error with code and message
struct Error {
    ErrorCode code = ErrorCode::None;
    std::string message;

    Error() = default;
    Error(ErrorCode c, std::string msg)
        : code(c)
        , message(std::move(msg))
    {
    }

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

    /// Create an error with a message
    [[nodiscard]]
    static auto make(ErrorCode c, std::string msg) -> Error
    {
        return Error { c, std::move(msg) };
    }

    /// Create an error with a string_view message
    [[nodiscard]]
    static auto make(ErrorCode c, std::string_view msg) -> Error
    {
        return Error { c, std::string { msg } };
    }
};

/// Result type alias for operations that may fail
template<typename T>
using Result = pup::expected<T, Error>;

/// Unit type for Result<void> equivalent
struct Unit { };

/// Success value for Result<Unit>
inline constexpr auto unit = Unit {};

/// Helper to create an error result
template<typename T, typename Msg>
requires std::is_convertible_v<Msg, std::string_view>
[[nodiscard]]
auto make_error(ErrorCode code, Msg&& msg) -> Result<T>
{
    return pup::unexpected<Error>(Error::make(code, std::string { std::forward<Msg>(msg) }));
}

} // namespace pup
