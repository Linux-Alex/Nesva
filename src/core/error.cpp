// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#include "nesva/core/error.hpp"

#include <utility>

namespace nesva {

std::string_view toToken(ErrorCode code) noexcept {
    switch (code) {
    case ErrorCode::Ok:
        return "ok";
    case ErrorCode::InvalidArgument:
        return "invalid_argument";
    case ErrorCode::NotFound:
        return "not_found";
    case ErrorCode::AlreadyExists:
        return "already_exists";
    case ErrorCode::PermissionDenied:
        return "permission_denied";
    case ErrorCode::IoError:
        return "io_error";
    case ErrorCode::DatabaseError:
        return "database_error";
    case ErrorCode::ConfigurationError:
        return "configuration_error";
    case ErrorCode::Cancelled:
        return "cancelled";
    case ErrorCode::ExternalToolFailed:
        return "external_tool_failed";
    case ErrorCode::Timeout:
        return "timeout";
    case ErrorCode::Unsupported:
        return "unsupported";
    case ErrorCode::NotImplemented:
        return "not_implemented";
    case ErrorCode::IntegrityError:
        return "integrity_error";
    case ErrorCode::Internal:
        return "internal_error";
    case ErrorCode::DestinationUnavailable:
        return "destination_unavailable";
    }
    return "internal_error";
}

Error::Error(ErrorCode code, std::string message, std::string detail)
    : code_(code), message_(std::move(message)), detail_(std::move(detail)) {}

std::string Error::toDisplayString() const {
    if (detail_.empty()) {
        return message_;
    }
    return message_ + " (" + detail_ + ")";
}

} // namespace nesva
