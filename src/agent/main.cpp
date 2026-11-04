// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

// nesva-agent: background execution of scheduled archive jobs.
//
// It runs independently of the desktop window, exposes a local socket for the
// GUI and CLI, and shuts down gracefully on SIGINT/SIGTERM, finishing the file
// in flight before exiting.

#include <QtCore/QCoreApplication>
#include <QtCore/QTimer>

#include <csignal>
#include <cstdio>
#include <optional>

#if defined(Q_OS_WIN)
// windows.h only for the console attach below; nothing here touches the registry.
#include <windows.h>
#endif

#include "agent_service.hpp"
#include "nesva/core/app_info.hpp"
#include "nesva/core/logging.hpp"

namespace {

volatile std::sig_atomic_t g_stopRequested = 0;
volatile std::sig_atomic_t g_reloadRequested = 0;

void handleStopSignal(int) {
    g_stopRequested = 1;
}

#if defined(Q_OS_WIN)
/// The agent is a GUI-subsystem binary so the logon Run entry does not open a
/// console window in the user's session. When it is nevertheless started from a
/// terminal, attach to that console so --help, --version and --verbose print
/// where the user is looking. Started by the shell there is no console to attach
/// to, and then this does nothing at all.
void attachParentConsole() {
    if (::AttachConsole(ATTACH_PARENT_PROCESS) == 0) {
        return;
    }
    FILE* stream = nullptr;
    if (freopen_s(&stream, "CONOUT$", "w", stdout) != 0) {
        return;
    }
    freopen_s(&stream, "CONOUT$", "w", stderr);
}
#endif

#if defined(SIGHUP)
// Only compiled where the reload signal exists (POSIX); on Windows the agent
// reloads through its own schedule changes.
void handleReloadSignal(int) {
    g_reloadRequested = 1;
}
#endif

QString helpText() {
    return QStringLiteral(
        "nesva-agent - Nesva background scheduler and runner\n"
        "\n"
        "Usage: nesva-agent [options]\n"
        "\n"
        "Options:\n"
        "  --data-dir <path>   Override the per-user data directory\n"
        "  --tick <seconds>    Scheduling tick interval (default 20)\n"
        "  --once              Run one scheduling pass and exit when idle\n"
        "  --verbose           Log to the console\n"
        "  --version           Print version information\n"
        "  --help              Print this help\n"
        "\n"
        "Signals: SIGINT/SIGTERM stop after the current file; SIGHUP reloads schedules.\n"
        "Control: nesva-cli agent status|run|cancel|stop, or the desktop application.\n");
}

} // namespace

int main(int argc, char** argv) {
#if defined(Q_OS_WIN)
    attachParentConsole();
#endif
    QCoreApplication application(argc, argv);
    nesva::initialiseApplicationMetadata();

    nesva::agent::AgentOptions options;
    bool showHelp = false;
    bool showVersion = false;

    QStringList arguments;
    for (int index = 1; index < argc; ++index) {
        arguments.append(QString::fromLocal8Bit(argv[index]));
    }
    for (int index = 0; index < arguments.size(); ++index) {
        const QString& argument = arguments.at(index);
        auto valueOf = [&](const QString& inlineValue) -> QString {
            if (!inlineValue.isEmpty()) {
                return inlineValue;
            }
            if (index + 1 < arguments.size()) {
                return arguments.at(++index);
            }
            return QString();
        };
        if (argument == QStringLiteral("--help") || argument == QStringLiteral("-h")) {
            showHelp = true;
        } else if (argument == QStringLiteral("--version") || argument == QStringLiteral("-V")) {
            showVersion = true;
        } else if (argument == QStringLiteral("--verbose")) {
            options.consoleLogging = true;
        } else if (argument == QStringLiteral("--once")) {
            options.runOnce = true;
        } else if (argument.startsWith(QStringLiteral("--data-dir"))) {
            options.dataRootOverride = valueOf(argument.section(QLatin1Char('='), 1));
        } else if (argument.startsWith(QStringLiteral("--tick"))) {
            bool ok = false;
            const int seconds = valueOf(argument.section(QLatin1Char('='), 1)).toInt(&ok);
            if (!ok || seconds < 1 || seconds > 3600) {
                std::fprintf(stderr, "error: --tick must be between 1 and 3600 seconds\n");
                return 1;
            }
            options.tickIntervalMs = seconds * 1000;
        }
    }

    if (showVersion) {
        std::printf("%s (nesva-agent)\ncommit: %s\nQt: %s\n",
                    qPrintable(nesva::ApplicationInfo::displayVersion()),
                    qPrintable(nesva::ApplicationInfo::gitCommit()),
                    qPrintable(nesva::ApplicationInfo::qtVersion()));
        return 0;
    }
    if (showHelp) {
        const QByteArray text = helpText().toUtf8();
        std::fwrite(text.constData(), 1, static_cast<std::size_t>(text.size()), stdout);
        return 0;
    }

    std::signal(SIGINT, handleStopSignal);
    std::signal(SIGTERM, handleStopSignal);
#if defined(SIGHUP)
    // POSIX only: Windows has no SIGHUP, so there is nothing to reload with
    // there - the agent reloads on its own schedule changes.
    std::signal(SIGHUP, handleReloadSignal);
#endif

    nesva::agent::AgentService service(options);
    const nesva::Status initialised = service.initialise();
    if (!initialised) {
        std::fprintf(stderr, "nesva-agent: %s\n",
                     initialised.error().toDisplayString().c_str());
        return 1;
    }

    int exitCode = 0;
    QObject::connect(&service, &nesva::agent::AgentService::stopped,
                     &application, [&application, &exitCode](int code) {
                         exitCode = code;
                         QCoreApplication::exit(code);
                     });

    // Signal flags are polled from the event loop: async-signal-safe and
    // portable across platforms.
    auto* signalTimer = new QTimer(&application);
    QObject::connect(signalTimer, &QTimer::timeout, &service, [&service, &application] {
        if (g_stopRequested != 0) {
            g_stopRequested = 0;
            service.requestStop();
        }
        if (g_reloadRequested != 0) {
            g_reloadRequested = 0;
            nesva::logInfo("agent", QStringLiteral("reloading schedules on request"));
            service.tick();
        }
        if (service.isStopping()) {
            return;
        }
        Q_UNUSED(application);
    });
    signalTimer->start(250);

    service.tick();


    // Headless single pass: start the tick loop, wait for the started run (if
    // any) to finish, then exit. Used by CI and by "run once" automation.
    if (options.runOnce) {
        QObject::connect(&service, &nesva::agent::AgentService::jobFinished, &application,
                         [&application](qint64, const nesva::jobs::RunReport&) {
                             QTimer::singleShot(0, &application, [] { QCoreApplication::exit(0); });
                         });

        auto* idleWatch = new QTimer(&application);
        idleWatch->setInterval(300);
        auto* consecutiveIdleChecks = new int(0);
        QObject::connect(idleWatch, &QTimer::timeout, &application,
                         [&service, &application, idleWatch, consecutiveIdleChecks] {
                             if (service.status().runningJobId != 0) {
                                 *consecutiveIdleChecks = 0;
                                 return;
                             }
                             // Two consecutive idle observations ensure a run that
                             // was just scheduled is not skipped.
                             if (++(*consecutiveIdleChecks) >= 2) {
                                 idleWatch->stop();
                                 QCoreApplication::exit(0);
                             }
                         });
        idleWatch->start();
    }

    const int code = QCoreApplication::exec();
    return exitCode != 0 ? exitCode : code;
}
