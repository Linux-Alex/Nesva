// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#pragma once

#include <QtCore/QString>
#include <QtCore/QStringList>

#include "nesva/core/result.hpp"

namespace nesva {

/// Result of inspecting an external tool binary (Rclone, Restic).
struct ExternalToolInfo {
    QString executable;   ///< Resolved absolute path actually executed.
    QString versionText;  ///< First non-empty line of the version output.
    int exitCode = 0;
};

/// Locates and version-probes an external binary without ever involving a
/// shell. Arguments are passed as an array; the caller cannot inject shell
/// metacharacters.
///
/// \param executable        Absolute path, or a bare name resolved on PATH.
/// \param versionArguments  Arguments that print the version (for example {"version"}).
/// \param timeoutMs         Hard timeout for start and finish.
Result<ExternalToolInfo> probeExternalTool(const QString& executable,
                                           const QStringList& versionArguments,
                                           int timeoutMs = 5000);

/// Returns the absolute path of an executable, or an empty string when it is
/// not found. Accepts absolute paths and bare names.
QString resolveExecutable(const QString& executable);

} // namespace nesva
