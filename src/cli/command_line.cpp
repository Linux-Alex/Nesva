// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#include "command_line.hpp"

#include <QtCore/QCoreApplication>

#include "nesva/core/app_info.hpp"

namespace nesva::cli {
namespace {

const QStringList& knownCommands() {
    static const QStringList commands = {
        QStringLiteral("status"),  QStringLiteral("doctor"),  QStringLiteral("jobs"),
        QStringLiteral("storage"), QStringLiteral("history"), QStringLiteral("archive"),
        QStringLiteral("verify"),  QStringLiteral("agent"),   QStringLiteral("schedule"),
        QStringLiteral("repository"), QStringLiteral("snapshots"), QStringLiteral("retention"),
        QStringLiteral("mail"),    QStringLiteral("tools"),
        QStringLiteral("backup"),  QStringLiteral("version"), QStringLiteral("help"),
    };
    return commands;
}

} // namespace

namespace {

/// Flags that never take a separate value; every other option accepts both
/// "--flag value" and "--flag=value".
const QStringList& booleanFlags() {
    static const QStringList flags = {QStringLiteral("json"),    QStringLiteral("verbose"),
                                      QStringLiteral("quiet"),   QStringLiteral("help"),
                                      QStringLiteral("version"), QStringLiteral("no-console-log")};
    return flags;
}

} // namespace

Options parseArguments(const QStringList& arguments) {
    Options options;
    QStringList remaining;

    for (int index = 0; index < arguments.size(); ++index) {
        const QString& argument = arguments.at(index);
        if (argument == QStringLiteral("--json")) {
            options.json = true;
        } else if (argument == QStringLiteral("--verbose") || argument == QStringLiteral("-v")) {
            options.verbose = true;
        } else if (argument == QStringLiteral("--quiet") || argument == QStringLiteral("-q")) {
            options.quiet = true;
        } else if (argument == QStringLiteral("--help") || argument == QStringLiteral("-h")) {
            options.helpRequested = true;
        } else if (argument == QStringLiteral("--version") || argument == QStringLiteral("-V")) {
            options.versionRequested = true;
        } else if (argument.startsWith(QStringLiteral("--"))) {
            const QString body = argument.mid(2);
            const qsizetype equals = body.indexOf(QLatin1Char('='));
            if (equals >= 0) {
                options.flags.insert(body.left(equals).toLower(), body.mid(equals + 1));
            } else if (!booleanFlags().contains(body.toLower()) && index + 1 < arguments.size() &&
                       !arguments.at(index + 1).startsWith(QLatin1Char('-'))) {
                options.flags.insert(body.toLower(), arguments.at(++index));
            } else {
                options.flags.insert(body.toLower(), QString());
            }
        } else {
            remaining.append(argument);
        }
    }

    if (!remaining.isEmpty()) {
        options.command = remaining.takeFirst();
    }
    if (!remaining.isEmpty() && !remaining.constFirst().startsWith(QLatin1Char('-'))) {
        const QString candidate = remaining.constFirst();
        // The first positional word is a subcommand only when it is one this
        // list recognises; otherwise it is an argument (a job id, an account
        // id, ...). Keep it in sync with usageText().
        static const QStringList subcommands = {
            "accounts", "add",      "apply",     "cancel",   "check",    "connect", "estimate",
            "folders",  "help",     "info",      "init",     "list",     "locks",   "preview",
            "rclone",   "reload",   "restore",   "run",      "set-password", "show", "status",
            "stop",     "sync",     "test",      "unlock"};
        if (subcommands.contains(candidate)) {
            options.subcommand = remaining.takeFirst();
        }
    }
    options.positional = remaining;

    if (!options.command.isEmpty() && !knownCommands().contains(options.command)) {
        options.parseErrors.append(
            QStringLiteral("unknown command: %1").arg(options.command));
    }

    return options;
}

QString versionText() {
    return QStringLiteral("%1\ncommit: %2\nQt: %3")
        .arg(ApplicationInfo::displayVersion(), ApplicationInfo::gitCommit(),
             ApplicationInfo::qtVersion());
}

QString usageText() {
    return QStringLiteral(
        "Nesva command line interface\n"
        "\n"
        "Usage: nesva-cli [global options] <command> [command options]\n"
        "\n"
        "Global options:\n"
        "  --json            Emit machine-readable JSON instead of text\n"
        "  --verbose, -v     Increase console log verbosity\n"
        "  --quiet, -q       Emit no informational console output\n"
        "  --help, -h        Show this help\n"
        "  --version, -V     Show version information\n"
        "\n"
        "Commands:\n"
        "  status                    Overall application, database and storage status\n"
        "  doctor                    Environment and dependency diagnostics\n"
        "  jobs list                 List configured jobs\n"
        "  jobs run <job-id>         Run an archive job now\n"
        "                            options: --dry-run, --force, --retries <n>\n"
        "  jobs cancel <job-id>      Recover a run whose owner process is gone\n"
        "  storage list              List configured storage providers\n"
        "  storage test <id>         Test a storage connection\n"
        "  archive list <job-id>     List the archived objects of a job\n"
        "  archive restore <job-id>  Restore into a directory\n"
        "                            options: --to <dir>, --policy skip|overwrite|rename,\n"
        "                                     --overwrite, --rename, --no-verify\n"
        "  history list              List recent job runs (--limit <n>)\n"
        "  mail connect              Sign in to Microsoft 365 and store the token in the keyring\n"
        "                            (--account <upn> [--tenant <id>] [--client <id>])\n"
        "  mail accounts             List connected Microsoft 365 accounts\n"
        "  mail folders <account>    List folders ([--enable|--disable <remote-id>])\n"
        "  mail estimate <account>   Count the mail per folder and estimate the sync time\n"
        "  mail sync <account>       Export mail as .eml (--to <dir> [--full] [--folder <id>])\n"
        "                            (--max-messages <n>, --quiet to silence progress)\n"
        "  mail status <account>     Export counters for an account\n"
        "  repository info <job>     Show where the repository password is stored\n"
        "  repository init <job>     Store the repository password and run 'restic init'\n"
        "                            (--password-stdin | --password-file <path>)\n"
        "  repository check <job>    Verify repository integrity (--read-data 5%)\n"
        "  repository locks <job>    List repository locks\n"
        "  repository unlock <job>   Remove stale locks\n"
        "  snapshots list <job>      List Restic snapshots of a backup job\n"
        "  retention preview <job>   Show what the retention policy would remove\n"
        "  retention apply <job>     Apply retention (--prune to reclaim space)\n"
        "  backup restore <job>      Restore a snapshot\n"
        "                            (--snapshot <id|latest>, --to <dir>, --policy ...)\n"
        "  schedule list             List job schedules and their next run\n"
        "  agent status              Background agent state (exit 3 when stopped)\n"
        "  agent run <job-id>        Ask the agent to run a job now\n"
        "  agent cancel <job-id>     Ask the agent to cancel the running job\n"
        "  agent reload              Reload schedules in the agent\n"
        "  agent stop                Stop the agent gracefully\n"
        "  verify <job-id>           Re-check archived objects against the manifest\n"
        "  version                   Show version information\n"
        "  help                      Show this help\n"
        "\n"
        "Exit codes:\n"
        "  0 success, 1 usage error, 2 runtime failure, 3 not found, 4 not implemented\n");
}

} // namespace nesva::cli
