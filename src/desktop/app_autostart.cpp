// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#include "app_autostart.hpp"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonObject>
#include <QtCore/QSaveFile>
#include <QtCore/QStandardPaths>

#if defined(Q_OS_WIN)
#include <QtCore/QSettings>
#endif

#include "nesva/core/logging.hpp"

namespace nesva::ui {
namespace {

QString applicationPath() {
    return QFileInfo(QCoreApplication::applicationFilePath()).absoluteFilePath();
}

#if defined(Q_OS_WIN)
constexpr const char* kRunValueName = "Nesva";

QString runKeyPath() {
    return QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run");
}
#elif defined(Q_OS_LINUX)
QString desktopFilePath() {
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) +
           QStringLiteral("/autostart/nesva.desktop");
}

/// Escape one Exec argument per the Desktop Entry specification: inside double
/// quotes a backslash must escape backslash, double quote, backtick and dollar.
QString quotedExecutable(const QString& path) {
    QString escaped = path;
    escaped.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    escaped.replace(QLatin1Char('"'), QStringLiteral("\\\""));
    escaped.replace(QChar(0x60), QString(QChar('\\')) + QChar(0x60));
    escaped.replace(QLatin1Char('$'), QStringLiteral("\\$"));
    return QLatin1Char('"') + escaped + QLatin1Char('"');
}

QString desktopEntry() {
    return QStringLiteral("[Desktop Entry]\n"
                          "Type=Application\n"
                          "Name=Nesva\n"
                          "Comment=Backup and archival application\n"
                          "Exec=%1 %2\n"
                          "Icon=nesva\n"
                          "Terminal=false\n"
                          "X-GNOME-Autostart-enabled=true\n")
        .arg(quotedExecutable(applicationPath()), QString::fromLatin1(kStartInTrayArgument));
}
#endif

} // namespace

QString appAutostartCommand() {
    return QStringLiteral("\"%1\" %2")
        .arg(QDir::toNativeSeparators(applicationPath()),
             QString::fromLatin1(kStartInTrayArgument));
}

bool appAutostartInstalled() {
#if defined(Q_OS_WIN)
    QSettings runKey(runKeyPath(), QSettings::NativeFormat);
    const QString value = runKey.value(QString::fromLatin1(kRunValueName)).toString().trimmed();
    return !value.isEmpty();
#elif defined(Q_OS_LINUX)
    return QFile::exists(desktopFilePath());
#else
    return false;
#endif
}

Status installAppAutostart() {
#if defined(Q_OS_WIN)
    QSettings runKey(runKeyPath(), QSettings::NativeFormat);
    runKey.setValue(QString::fromLatin1(kRunValueName), appAutostartCommand());
    runKey.sync();
    if (runKey.status() != QSettings::NoError) {
        return makeError(ErrorCode::PermissionDenied,
                         "cannot write the per-user autostart registry value");
    }
    logInfo("app.autostart",
            QStringLiteral("registered the desktop application at logon"),
            QJsonObject{{QStringLiteral("command"), appAutostartCommand()}});
    return Status();
#elif defined(Q_OS_LINUX)
    const QString path = desktopFilePath();
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        return makeError(ErrorCode::IoError,
                         "cannot create the autostart directory",
                         QFileInfo(path).absolutePath().toStdString());
    }
    QSaveFile file(path);
    const QByteArray contents = desktopEntry().toUtf8();
    if (!file.open(QIODevice::WriteOnly) || file.write(contents) != contents.size() ||
        !file.commit()) {
        return makeError(
            ErrorCode::IoError, "cannot write the autostart entry", path.toStdString());
    }
    logInfo("app.autostart",
            QStringLiteral("registered the desktop application at logon"),
            QJsonObject{{QStringLiteral("file"), path}});
    return Status();
#else
    return makeError(ErrorCode::Unsupported, "desktop autostart is not supported on this platform");
#endif
}

Status removeAppAutostart() {
#if defined(Q_OS_WIN)
    QSettings runKey(runKeyPath(), QSettings::NativeFormat);
    runKey.remove(QString::fromLatin1(kRunValueName));
    runKey.sync();
    if (runKey.status() != QSettings::NoError) {
        return makeError(ErrorCode::PermissionDenied,
                         "cannot remove the per-user autostart registry value");
    }
    logInfo("app.autostart", QStringLiteral("removed the desktop application autostart entry"));
    return Status();
#elif defined(Q_OS_LINUX)
    const QString path = desktopFilePath();
    if (!QFile::exists(path)) {
        return Status();
    }
    if (!QFile::remove(path)) {
        return makeError(
            ErrorCode::IoError, "cannot remove the autostart entry", path.toStdString());
    }
    logInfo("app.autostart",
            QStringLiteral("removed the desktop application autostart entry"),
            QJsonObject{{QStringLiteral("file"), path}});
    return Status();
#else
    return makeError(ErrorCode::Unsupported, "desktop autostart is not supported on this platform");
#endif
}

QString appAutostartDescription() {
    if (!appAutostartInstalled()) {
        return QStringLiteral("Not installed");
    }
#if defined(Q_OS_WIN)
    QSettings runKey(runKeyPath(), QSettings::NativeFormat);
    return QStringLiteral("Installed for this user: %1")
        .arg(runKey.value(QString::fromLatin1(kRunValueName)).toString());
#elif defined(Q_OS_LINUX)
    return QStringLiteral("Installed for this user: %1").arg(desktopFilePath());
#else
    return QStringLiteral("Not supported on this platform");
#endif
}

} // namespace nesva::ui
