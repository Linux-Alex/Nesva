// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#pragma once

#include <QtCore/QString>
#include <QtCore/QStringList>

#include "nesva/core/result.hpp"

namespace nesva::tools {

/// Launches a program in a new interactive terminal window.
///
/// Used for tools whose only interface is interactive - \c rclone \c config asks
/// questions and cannot be driven from Nesva. The terminal is chosen from the
/// ones actually installed; the program is started as a normal argument vector,
/// never through a shell.
Result<QString> openInTerminal(const QString& program, const QStringList& arguments);

} // namespace nesva::tools
