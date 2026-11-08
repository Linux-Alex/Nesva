// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QTimer>
#include <QtWidgets/QApplication>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QStyleFactory>

#include <cstdio>
#include <optional>

#include "app_autostart.hpp"
#include "app_context.hpp"
#include "gui_selftest.hpp"
#include "icon_factory.hpp"
#include "main_window.hpp"
#include "single_instance.hpp"
#include "nesva/core/app_info.hpp"
#include "nesva/core/logging.hpp"
#include "nesva/core/paths.hpp"
#include "theme.hpp"

namespace {

struct Options {
    QString dataRoot;
    std::optional<nesva::LogLevel> consoleLogLevel;
    bool consoleLogging = true;
    bool showHelp = false;
    bool showVersion = false;
    bool selfTest = false;
    bool startInTray = false;
    QString selfTestDirectory;
};

QString helpText() {
    return QStringLiteral(
        "Nesva desktop application\n"
        "\n"
        "Usage: nesva [options]\n"
        "\n"
        "Options:\n"
        "  --data-dir <path>      Override the per-user data directory\n"
        "  --log-level <level>    Console log level: trace, debug, info, warn, error\n"
        "  --no-console-log       Write logs to the log file only\n"
        "  --tray                 Start hidden in the notification area\n"
        "  --gui-selftest[=<dir>] Show the window, capture every page as PNG and exit\n"
        "  --version              Print version information\n"
        "  --help                 Print this help\n"
        "\n"
        "Data directory: resolved with --data-dir, then NESVA_DATA_DIR, then the\n"
        "platform default. Nothing is written next to the executable.\n");
}

Options parseOptions(const QStringList& arguments, QStringList* errors) {
    Options options;
    for (int index = 0; index < arguments.size(); ++index) {
        const QString argument = arguments.at(index);
        auto takeValue = [&](const QString& inlineValue) -> QString {
            if (!inlineValue.isEmpty()) {
                return inlineValue;
            }
            if (index + 1 < arguments.size()) {
                return arguments.at(++index);
            }
            return QString();
        };

        if (argument == QStringLiteral("--help") || argument == QStringLiteral("-h")) {
            options.showHelp = true;
        } else if (argument == QStringLiteral("--version") || argument == QStringLiteral("-V")) {
            options.showVersion = true;
        } else if (argument == QStringLiteral("--no-console-log")) {
            options.consoleLogging = false;
        } else if (argument == QString::fromLatin1(nesva::ui::kStartInTrayArgument)) {
            options.startInTray = true;
        } else if (argument.startsWith(QStringLiteral("--data-dir"))) {
            const QString value = takeValue(argument.section(QLatin1Char('='), 1));
            if (value.isEmpty()) {
                errors->append(QStringLiteral("--data-dir requires a path"));
            } else {
                options.dataRoot = value;
            }
        } else if (argument.startsWith(QStringLiteral("--log-level"))) {
            const QString value = takeValue(argument.section(QLatin1Char('='), 1));
            if (const auto level = nesva::logLevelFromString(value.toStdString())) {
                options.consoleLogLevel = *level;
            } else {
                errors->append(QStringLiteral("unknown log level: %1").arg(value));
            }
        } else if (argument == QStringLiteral("--gui-selftest") ||
                   argument.startsWith(QStringLiteral("--gui-selftest="))) {
            options.selfTest = true;
            options.selfTestDirectory = takeValue(argument.section(QLatin1Char('='), 1));
        } else if (!argument.isEmpty()) {
            errors->append(QStringLiteral("unknown option: %1").arg(argument));
        }
    }
    return options;
}

} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    nesva::initialiseApplicationMetadata();

    if (auto* style = QStyleFactory::create(QStringLiteral("Fusion")); style != nullptr) {
        QApplication::setStyle(style);
    }
    QApplication::setWindowIcon(nesva::ui::applicationIcon());

    QStringList arguments;
    arguments.reserve(argc > 0 ? argc - 1 : 0);
    for (int index = 1; index < argc; ++index) {
        arguments.append(QString::fromLocal8Bit(argv[index]));
    }

    QStringList errors;
    const Options options = parseOptions(arguments, &errors);

    if (options.showVersion) {
        const QByteArray text = (nesva::ApplicationInfo::displayVersion() + QLatin1Char('\n') +
                                 QStringLiteral("commit: %1\nQt: %2\n")
                                     .arg(nesva::ApplicationInfo::gitCommit(),
                                          nesva::ApplicationInfo::qtVersion()))
                                    .toUtf8();
        std::fwrite(text.constData(), 1, static_cast<std::size_t>(text.size()), stdout);
        return 0;
    }
    if (options.showHelp) {
        const QByteArray text = helpText().toUtf8();
        std::fwrite(text.constData(), 1, static_cast<std::size_t>(text.size()), stdout);
        return 0;
    }
    if (!errors.isEmpty()) {
        const QByteArray text = (QStringLiteral("error: %1\n\n").arg(errors.first()) + helpText()).toUtf8();
        std::fwrite(text.constData(), 1, static_cast<std::size_t>(text.size()), stderr);
        return 1;
    }

    // Resolve the data directory before opening the database: the single
    // instance channel is scoped to it, so two profiles can still run side by
    // side while a second launch of the same profile reuses the running window.
    if (!options.dataRoot.isEmpty()) {
        nesva::Paths::setDataRootOverride(options.dataRoot);
    }

    nesva::ui::SingleInstance singleInstance;
    if (!options.selfTest) {
        if (singleInstance.notifyExistingInstance()) {
            return 0;
        }
        if (!singleInstance.listen()) {
            // A simultaneous start may have claimed the socket between the
            // notify and listen attempts; try to reach it once more.
            if (singleInstance.notifyExistingInstance()) {
                return 0;
            }
            const QString message =
                QStringLiteral("Nesva is already running, but its window could not be reached: %1")
                    .arg(singleInstance.errorString());
            std::fprintf(stderr, "%s\n", qPrintable(message));
            QMessageBox::critical(nullptr, QString::fromUtf8(nesva::kApplicationName), message);
            return 1;
        }
    }

    nesva::ui::AppContext context;
    nesva::ui::StartupOptions startup;
    startup.dataRootOverride = options.dataRoot;
    startup.consoleLogLevel = options.consoleLogLevel;
    startup.consoleLogging = options.consoleLogging;

    const nesva::Status initialised = context.initialise(startup);
    if (!initialised) {
        const QString message =
            QStringLiteral("Nesva could not start: %1")
                .arg(QString::fromStdString(initialised.error().toDisplayString()));
        std::fprintf(stderr, "%s\n", qPrintable(message));
        if (!options.selfTest) {
            QMessageBox::critical(nullptr, QString::fromUtf8(nesva::kApplicationName), message);
        }
        return 1;
    }

    nesva::ui::applyTheme(application, context.themeMode());

    nesva::ui::MainWindow window(context);
    QObject::connect(&context, &nesva::ui::AppContext::themeModeChanged, &window,
                     &nesva::ui::MainWindow::applyTheme);
    QObject::connect(
        &singleInstance, &nesva::ui::SingleInstance::showRequested, &window,
        [&context, &window] {
            context.recordEvent(nesva::LogLevel::Info, QStringLiteral("application"),
                                QStringLiteral("another instance asked for the window"));
            window.showFromTray();
        });
    QObject::connect(
        &singleInstance, &nesva::ui::SingleInstance::quitRequested, &window,
        [&context] {
            // "nesva-cli update --install" asks the window to step aside before the
            // installer replaces the executables it is running from.
            context.recordEvent(nesva::LogLevel::Info, QStringLiteral("application"),
                                QStringLiteral("quitting for an update"));
            QCoreApplication::quit();
        });

    if (options.selfTest) {
        QString directory = options.selfTestDirectory;
        if (directory.isEmpty()) {
            directory = QDir(nesva::Paths::cacheDirectory())
                            .absoluteFilePath(QStringLiteral("gui-selftest"));
        }
        return nesva::ui::runGuiSelfTest(window, directory);
    }

    if (options.startInTray && window.trayAvailable()) {
        // The tray icon was created by the MainWindow constructor; do not show
        // the window. This is how the per-user autostart entry starts Nesva.
        context.recordEvent(nesva::LogLevel::Info, QStringLiteral("application"),
                            QStringLiteral("started in the notification area"));
    } else {
        if (options.startInTray) {
            std::fprintf(stderr,
                         "nesva: the system tray is not available; showing the window instead\n");
        }
        window.show();
    }
    return QApplication::exec();
}
