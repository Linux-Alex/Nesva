// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#include "nesva/agent/autostart.hpp"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonObject>
#include <QtCore/QProcess>
#include <QtCore/QSaveFile>
#include <QtCore/QSettings>

#include "nesva/core/logging.hpp"

#if defined(Q_OS_WIN)
#include <windows.h>
#endif

namespace nesva::agent {
namespace {

constexpr const char* kRunValueName = "NesvaAgent";

#if defined(Q_OS_WIN)
QString runKeyPath() {
    return QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run");
}
#else
QString unitDirectory() {
    const QString config = qEnvironmentVariable("XDG_CONFIG_HOME");
    const QString base = config.isEmpty() ? QDir::homePath() + QStringLiteral("/.config") : config;
    return base + QStringLiteral("/systemd/user");
}

QString unitPath() {
    return unitDirectory() + QStringLiteral("/nesva-agent.service");
}

QString unitContents(const QString& executable) {
    return QStringLiteral(
               "[Unit]\n"
               "Description=Nesva background backup agent\n"
               "# User-level unit: it runs inside the user's session so cloud credentials and\n"
               "# the keyring stay available. Never install this as a system-wide service.\n"
               "\n"
               "[Service]\n"
               "Type=simple\n"
               "ExecStart=%1\n"
               "Restart=on-failure\n"
               "RestartSec=30\n"
               "KillSignal=SIGTERM\n"
               "TimeoutStopSec=120\n"
               "NoNewPrivileges=true\n"
               "PrivateTmp=true\n"
               "ProtectSystem=strict\n"
               "ProtectHome=read-only\n"
               "ReadWritePaths=%2\n"
               "Environment=QT_QPA_PLATFORM=offscreen\n"
               "Environment=\"PATH=%3:/usr/local/bin:/usr/bin:/bin\"\n"
               "\n"
               "[Install]\n"
               "WantedBy=default.target\n")
        .arg(executable, QDir::homePath() + QStringLiteral("/.local/share/nesva"),
             QFileInfo(executable).absolutePath());
}

int runSystemctl(const QStringList& arguments, QString* error) {
    QProcess process;
    process.start(QStringLiteral("systemctl"),
                  QStringList{QStringLiteral("--user")} + arguments);
    if (!process.waitForStarted(5000)) {
        *error = QStringLiteral("systemctl is not available");
        return -1;
    }
    process.waitForFinished(30000);
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        *error = QString::fromLocal8Bit(process.readAllStandardError()).trimmed();
        return process.exitCode();
    }
    return 0;
}
#endif

} // namespace

Result<QString> installAutostart(const QString& agentExecutable) {
    if (agentExecutable.trimmed().isEmpty() || !QFileInfo::exists(agentExecutable)) {
        return makeErrorValue(ErrorCode::NotFound, "nesva-agent executable not found",
                              agentExecutable.toStdString());
    }
    const QString executable = QFileInfo(agentExecutable).absoluteFilePath();

#if defined(Q_OS_WIN)
    // Per-user autostart through the Run key: no PowerShell, no schtasks, no
    // elevation, and it works under group policies that block scripting.
    QSettings runKey(runKeyPath(), QSettings::NativeFormat);
    runKey.setValue(QString::fromLatin1(kRunValueName), QDir::toNativeSeparators(executable));
    runKey.sync();
    if (runKey.status() != QSettings::NoError) {
        return makeErrorValue(ErrorCode::PermissionDenied,
                              "cannot write the per-user autostart registry value");
    }
    logInfo("agent.autostart", QStringLiteral("registered the agent at logon"),
            QJsonObject{{QStringLiteral("executable"), executable}});
    return QStringLiteral("NesvaAgent in HKCU\\...\\CurrentVersion\\Run -> %1").arg(executable);
#else
    if (!QDir().mkpath(unitDirectory())) {
        return makeErrorValue(ErrorCode::IoError, "cannot create the systemd user directory",
                              unitDirectory().toStdString());
    }
    QSaveFile unit(unitPath());
    const QByteArray contents = unitContents(executable).toUtf8();
    if (!unit.open(QIODevice::WriteOnly) || unit.write(contents) != contents.size() ||
        !unit.commit()) {
        return makeErrorValue(ErrorCode::IoError, "cannot write the systemd user unit",
                              unitPath().toStdString());
    }
    QString error;
    if (runSystemctl({QStringLiteral("daemon-reload")}, &error) != 0) {
        return makeErrorValue(ErrorCode::ExternalToolFailed, "systemctl daemon-reload failed",
                              error.toStdString());
    }
    if (runSystemctl({QStringLiteral("enable"), QStringLiteral("--now"),
                      QStringLiteral("nesva-agent.service")},
                     &error) != 0) {
        return makeErrorValue(ErrorCode::ExternalToolFailed, "cannot enable the agent service",
                              error.toStdString());
    }
    logInfo("agent.autostart", QStringLiteral("enabled the agent user service"),
            QJsonObject{{QStringLiteral("unit"), unitPath()}});
    return QStringLiteral("systemd user unit %1 (enabled, running)").arg(unitPath());
#endif
}

Result<QString> removeAutostart() {
#if defined(Q_OS_WIN)
    QSettings runKey(runKeyPath(), QSettings::NativeFormat);
    if (!runKey.contains(QString::fromLatin1(kRunValueName))) {
        return QStringLiteral("no autostart entry was registered");
    }
    runKey.remove(QString::fromLatin1(kRunValueName));
    runKey.sync();
    return QStringLiteral("removed NesvaAgent from the per-user Run key");
#else
    QString error;
    runSystemctl({QStringLiteral("disable"), QStringLiteral("--now"),
                  QStringLiteral("nesva-agent.service")},
                 &error);
    if (QFile::exists(unitPath()) && !QFile::remove(unitPath())) {
        return makeErrorValue(ErrorCode::IoError, "cannot remove the systemd user unit");
    }
    runSystemctl({QStringLiteral("daemon-reload")}, &error);
    return QStringLiteral("removed %1 and stopped the service").arg(unitPath());
#endif
}

QString autostartExecutable() {
#if defined(Q_OS_WIN)
    QSettings runKey(runKeyPath(), QSettings::NativeFormat);
    const QString value = runKey.value(QString::fromLatin1(kRunValueName)).toString().trimmed();
    return value;
#else
    QFile unit(unitPath());
    if (!unit.open(QIODevice::ReadOnly)) {
        return QString();
    }
    const QString text = QString::fromUtf8(unit.readAll());
    for (const QString& line : text.split(QLatin1Char('\n'))) {
        if (line.startsWith(QStringLiteral("ExecStart="))) {
            return line.mid(QStringLiteral("ExecStart=").size()).trimmed();
        }
    }
    return QString();
#endif
}

QString autostartDescription() {
    const QString executable = autostartExecutable();
    if (executable.isEmpty()) {
        return QStringLiteral("Not installed");
    }
    return QStringLiteral("Installed for this user: %1").arg(executable);
}

} // namespace nesva::agent
