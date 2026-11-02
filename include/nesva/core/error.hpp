// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#pragma once

#include <string>
#include <string_view>

namespace nesva {

/// Stable, machine-readable error classification used across Nesva.
///
/// The numeric values are part of the CLI/JSON contract and must not be
/// renumbered once released.
enum class ErrorCode : int {
    Ok = 0,
    InvalidArgument = 1,
    NotFound = 2,
    AlreadyExists = 3,
    PermissionDenied = 4,
    IoError = 5,
    DatabaseError = 6,
    ConfigurationError = 7,
    Cancelled = 8,
    ExternalToolFailed = 9,
    Timeout = 10,
    Unsupported = 11,
    NotImplemented = 12,
    IntegrityError = 13,
    Internal = 14,
};

/// Lower-case token used in logs and JSON output ("invalid_argument", ...).
std::string_view toToken(ErrorCode code) noexcept;

/// A recoverable failure. Error values are plain data and never contain
/// credentials: callers must redact sensitive strings before constructing one.
class Error {
public:
    Error() = default;
    Error(ErrorCode code, std::string message, std::string detail = {});

    ErrorCode code() const noexcept { return code_; }
    const std::string& message() const noexcept { return message_; }
    const std::string& detail() const noexcept { return detail_; }

    /// "message (detail)" when a detail is present, otherwise just the message.
    std::string toDisplayString() const;

private:
    ErrorCode code_ = ErrorCode::Internal;
    std::string message_;
    std::string detail_;
};

} // namespace nesva
