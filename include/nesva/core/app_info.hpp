// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#pragma once

#include <QtCore/QJsonObject>
#include <QtCore/QString>

namespace nesva {

inline constexpr const char* kApplicationName = "Nesva";
inline constexpr const char* kApplicationShortName = "nesva";
inline constexpr const char* kOrganisationName = "Nesva";
inline constexpr const char* kOrganisationDomain = "nesva.invalid";
inline constexpr const char* kDesktopExecutableName = "nesva";
inline constexpr const char* kCliExecutableName = "nesva-cli";
inline constexpr const char* kAgentExecutableName = "nesva-agent";

/// Sets QCoreApplication organisation/application/version metadata exactly once.
/// All three executables (desktop, CLI, agent) call this before anything else.
void initialiseApplicationMetadata();

/// Compile-time version information plus the runtime Qt version.
class ApplicationInfo {
public:
    static QString version();
    static QString displayVersion();
    static QString gitCommit();
    static QString qtVersion();
    static QString platformDescription();
    static QJsonObject toJson();
};

} // namespace nesva
