// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#pragma once

#include <QtCore/QMap>
#include <QtCore/QString>
#include <QtCore/QStringList>

namespace nesva::cli {

/// Exit codes are part of the automation contract of nesva-cli.
enum ExitCode : int {
    kExitSuccess = 0,
    kExitUsage = 1,
    kExitFailure = 2,
    kExitNotFound = 3,
    kExitNotImplemented = 4,
};

/// Parsed command line. Global flags are accepted before the command name so
/// that "nesva-cli --json jobs list" works as expected.
struct Options {
    bool json = false;
    bool verbose = false;
    bool quiet = false;
    bool helpRequested = false;
    bool versionRequested = false;
    QString command;
    QString subcommand;
    QStringList positional;
    QMap<QString, QString> flags;
    QStringList parseErrors;
};

Options parseArguments(const QStringList& arguments);

/// Full usage text, printed for --help and on usage errors.
QString usageText();
QString versionText();

} // namespace nesva::cli
