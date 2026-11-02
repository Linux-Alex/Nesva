// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#pragma once

#include <optional>
#include <utility>
#include <variant>

#include "nesva/core/error.hpp"

namespace nesva {

/// Holds either a value of type T or an Error. Nesva does not use exceptions
/// for expected failures; every fallible operation returns Result or Status.
template <typename T>
class Result {
public:
    using value_type = T;

    // NOLINTNEXTLINE(google-explicit-constructor) - implicit conversion keeps call sites readable
    Result(T value) : storage_(std::move(value)) {}
    // NOLINTNEXTLINE(google-explicit-constructor)
    Result(Error error) : storage_(std::move(error)) {}

    bool hasValue() const noexcept { return std::holds_alternative<T>(storage_); }
    explicit operator bool() const noexcept { return hasValue(); }

    T& value() & { return std::get<T>(storage_); }
    const T& value() const& { return std::get<T>(storage_); }
    T take() && { return std::move(std::get<T>(storage_)); }

    T& operator*() & { return value(); }
    const T& operator*() const& { return value(); }
    T* operator->() { return &value(); }
    const T* operator->() const { return &value(); }

    const Error& error() const { return std::get<Error>(storage_); }

    /// Returns the contained value, or the supplied fallback on failure.
    const T& valueOr(const T& fallback) const& { return hasValue() ? std::get<T>(storage_) : fallback; }
    T valueOr(T fallback) && { return hasValue() ? std::move(std::get<T>(storage_)) : std::move(fallback); }

private:
    std::variant<T, Error> storage_;
};

/// Result type for operations that either succeed or fail without producing a value.
class Status {
public:
    Status() noexcept = default;
    // NOLINTNEXTLINE(google-explicit-constructor) - enables "return Error(...);"
    Status(Error error) : error_(std::move(error)) {}

    bool ok() const noexcept { return !error_.has_value(); }
    explicit operator bool() const noexcept { return ok(); }

    const Error& error() const { return *error_; }

private:
    std::optional<Error> error_;
};

/// Convenience factory used throughout the code base.
inline Status makeError(ErrorCode code, std::string message, std::string detail = {}) {
    return Status(Error(code, std::move(message), std::move(detail)));
}

inline Error makeErrorValue(ErrorCode code, std::string message, std::string detail = {}) {
    return Error(code, std::move(message), std::move(detail));
}

} // namespace nesva
