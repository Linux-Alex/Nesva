// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#pragma once

#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QVector>

#include <functional>

#include "nesva/core/http.hpp"
#include "nesva/core/result.hpp"

namespace nesva::tools {

/// Where an installed tool should live. "User" needs no privileges at all and
/// is the default; "System" puts the binary on the shared PATH and therefore
/// needs elevation.
enum class InstallScope { User, System };

QString toToken(InstallScope scope);

/// One step of an installation, described before anything runs.
///
/// \c command is the exact argument vector Nesva will execute - never a shell
/// string - so it can be shown to the user verbatim. An empty \c command means
/// Nesva performs the step in-process (a download, a checksum check, a copy).
struct InstallStep {
    QString title;
    QStringList command;
    QString note;
};

/// Everything an installation would do, resolved but not executed.
struct ToolInstallPlan {
    QString tool = QStringLiteral("rclone");
    QString version;          ///< release version without the leading "v"
    QString downloadUrl;
    QString checksumUrl;
    QString archiveName;      ///< file name inside the release directory
    QString expectedSha256;   ///< empty until the checksum list is fetched
    QString targetPath;       ///< absolute path of the installed binary
    InstallScope scope = InstallScope::User;
    bool requiresElevation = false;
    /// False when this platform cannot be installed automatically and the user
    /// has to run the shown commands themselves.
    bool automatic = true;
    QVector<InstallStep> steps;
    QStringList notes; ///< honest caveats shown next to the plan
};

/// Resolves the current rclone release and describes what installing it would
/// do. Nothing is downloaded, written or executed.
///
/// \param transport HTTP transport used to read the published version.
Result<ToolInstallPlan> planRcloneInstall(InstallScope scope, net::HttpTransport& transport);

struct InstallOutcome {
    QString executablePath;
    QString version; ///< reported by the installed binary
    qint64 bytesDownloaded = 0;
    QString sha256;
};

/// Performs a plan. Progress is reported as short human-readable lines.
///
/// Every failure leaves the system as it was: the binary is downloaded and
/// verified in a private temporary directory and only moved into place once
/// its SHA-256 matches the published checksum.
///
/// param transport Optional transport; tests inject a scripted one. A null
///        pointer means "talk to the real download server".
Result<InstallOutcome> executeRcloneInstall(const ToolInstallPlan& plan,
                                            const std::function<void(const QString&)>& progress,
                                            net::HttpTransport* transport = nullptr);

/// Renders a plan as the text the user reviews before anything runs: numbered
/// steps, the exact argument vector of each executed command, and the caveats.
QString describeInstallPlan(const ToolInstallPlan& plan);

/// Absolute path of the rclone binary an install would create for a scope.
QString rcloneTargetPath(InstallScope scope);

/// Directory on the search path that a user-scope install writes to, or an
/// empty string when the platform has none.
QString userInstallDirectory();

} // namespace nesva::tools
