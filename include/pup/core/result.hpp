// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include "expected.hpp"
#include "pup/core/string.hpp"

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
    String message;

    Error() = default;
    Error(ErrorCode c, String msg)
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
    static auto make(ErrorCode c, String msg) -> Error
    {
        return Error { c, std::move(msg) };
    }

    /// Create an error with a string_view message
    [[nodiscard]]
    static auto make(ErrorCode c, std::string_view msg) -> Error
    {
        return Error { c, String { msg } };
    }
};

/// Result type alias for operations that may fail
template<typename T>
using Result = pup::expected<T, Error>;

/// Helper to create an error result
template<typename T, typename Msg>
requires std::is_convertible_v<Msg, std::string_view>
[[nodiscard]]
auto make_error(ErrorCode code, Msg&& msg) -> Result<T>
{
    return pup::unexpected<Error>(Error::make(code, String { std::string_view { std::forward<Msg>(msg) } }));
}

} // namespace pup
