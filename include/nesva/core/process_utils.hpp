// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#pragma once

#include <QtCore/QtGlobal>

namespace nesva {

/// True when a process with this identifier currently exists on this machine.
///
/// Used to distinguish a genuinely running job from a row left behind by a
/// crashed process, so stale runs can be marked "interrupted" instead of
/// blocking future runs forever. A process that exists but belongs to another
/// user is treated as alive (the caller has no permission to inspect it).
bool isProcessAlive(qint64 pid);

} // namespace nesva
