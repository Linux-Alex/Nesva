// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#pragma once

#include <QtCore/QString>

#include "nesva/core/result.hpp"

namespace nesva {

/// Resolves the per-user directories Nesva uses.
///
/// Layout (Linux, XDG):
///   $XDG_DATA_HOME/nesva            -> database, job state
///   $XDG_DATA_HOME/nesva/logs       -> structured JSON logs
///   $XDG_CACHE_HOME/nesva           -> bounded temporary/cache files
///
/// Layout (Windows):
///   %LOCALAPPDATA%/Nesva            -> database, job state
///   %LOCALAPPDATA%/Nesva/logs       -> structured JSON logs
///   %LOCALAPPDATA%/Nesva/cache      -> bounded temporary/cache files
///
/// Testability and portable installs: a data root may be overridden with
/// setDataRootOverride() or the NESVA_DATA_DIR environment variable. Nothing is
/// ever written into the application installation directory.
class Paths {
public:
    static QString dataRoot();
    static QString logsDirectory();
    static QString cacheDirectory();
    static QString databasePath();
    static QString logFilePath(const QString& fileName = QStringLiteral("nesva.log"));

    /// Creates the data, logs and cache directories. Safe to call repeatedly.
    static Status ensureBaseDirectories();

    /// Overrides the data root for tests and portable installs. Passing an
    /// empty string clears the override.
    static void setDataRootOverride(const QString& path);
    static QString dataRootOverride();

    /// Human-readable description of the resolution order, used by
    /// "nesva-cli doctor".
    static QString describeResolution();
};

} // namespace nesva
