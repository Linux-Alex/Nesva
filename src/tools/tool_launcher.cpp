// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#include "nesva/tools/tool_launcher.hpp"

#include <QtCore/QProcess>
#include <QtCore/QStandardPaths>

#if defined(Q_OS_WIN)
#include <windows.h>
#include <shellapi.h>
#endif

#include "nesva/core/external_tool.hpp"

namespace nesva::tools {

Result<QString> openInTerminal(const QString& program, const QStringList& arguments) {
    const QString resolved = resolveExecutable(program);
    if (resolved.isEmpty()) {
        return makeErrorValue(ErrorCode::NotFound, "the program to run was not found",
                              program.toStdString());
    }

#if defined(Q_OS_WIN)
    // A console window is the only interactive terminal Windows offers; the
    // command is fixed and user-visible, and nothing is parsed as a script.
    QString command = QDir::toNativeSeparators(resolved);
    for (const QString& argument : arguments) {
        command += QLatin1Char(' ') + argument;
    }
    const std::wstring wide = QStringLiteral("/k \"%1\"").arg(command).toStdWString();
    const std::wstring exe = QStringLiteral("cmd.exe").toStdWString();
    const HINSTANCE result =
        ::ShellExecuteW(nullptr, L"open", exe.c_str(), wide.c_str(), nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<qintptr>(result) <= 32) {
        return makeErrorValue(ErrorCode::ExternalToolFailed, "cannot open a console window");
    }
    return QStringLiteral("cmd.exe");
#else
    struct Terminal {
        const char* program;
        QStringList prefix;
    };
    static const QVector<Terminal> kTerminals = {
        {"x-terminal-emulator", {QStringLiteral("-e")}},
        {"gnome-terminal", {QStringLiteral("--")}},
        {"konsole", {QStringLiteral("-e")}},
        {"xfce4-terminal", {QStringLiteral("-x")}},
        {"alacritty", {QStringLiteral("-e")}},
        {"kitty", {}},
        {"xterm", {QStringLiteral("-e")}},
    };
    for (const Terminal& terminal : kTerminals) {
        const QString executable = QStandardPaths::findExecutable(QString::fromUtf8(terminal.program));
        if (executable.isEmpty()) {
            continue;
        }
        QStringList args = terminal.prefix;
        args.append(resolved);
        args.append(arguments);
        if (QProcess::startDetached(executable, args)) {
            return QString::fromUtf8(terminal.program);
        }
    }
    return makeErrorValue(
        ErrorCode::ExternalToolFailed, "no terminal emulator could be started",
        QStringLiteral("run manually: %1 %2").arg(resolved, arguments.join(QLatin1Char(' ')))
            .toStdString());
#endif
}

} // namespace nesva::tools
