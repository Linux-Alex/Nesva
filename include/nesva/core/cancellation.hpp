// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#pragma once

#include <atomic>
#include <memory>

namespace nesva {

/// Cooperative cancellation shared between the UI, the job engine and long
/// running transfers.
///
/// Copies of a token observe the same state, so a cancel request made on the
/// GUI thread is seen by a worker thread without locks. Cancellation is
/// cooperative: operations check it at safe points and return ErrorCode::Cancelled
/// instead of leaving partial state behind.
class CancellationToken {
public:
    CancellationToken() : state_(std::make_shared<State>()) {}

    void cancel() const noexcept { state_->cancelled.store(true, std::memory_order_relaxed); }

    bool isCancelled() const noexcept {
        return state_->cancelled.load(std::memory_order_relaxed);
    }

private:
    struct State {
        std::atomic<bool> cancelled{false};
    };

    std::shared_ptr<State> state_;
};

} // namespace nesva
